//
// Physical page allocator.
// Slab allocator, for chunks larger than one page.
//

#include "types.h"
#include "mmu.h"
#include "kernel.hh"
#include "spinlock.h"
#include "kalloc.hh"
#include "mtrace.h"
#include "cpu.hh"
#include "multiboot.hh"
#include "wq.hh"
#include "page_info.hh"
#include "kstream.hh"

static struct Mbmem mem[128];
static u64 nmem;
static u64 membytes, memmax;
percpu<kmem, percpu_safety::internal> kmems;
percpu<kmem, percpu_safety::internal> slabmem[slab_type_max];

extern char end[]; // first address after kernel loaded from ELF file
char *newend;

page_info *page_info_array;
std::size_t page_info_len;
paddr page_info_base;

static int kinited __mpalign__;

static struct Mbmem *
memsearch(paddr pa)
{
  struct Mbmem *e;
  struct Mbmem *q;

  q = mem+nmem;
  for (e = &mem[0]; e < q; e++)
    if ((e->base <= pa) && ((e->base+e->length) > pa))
      return e;
  return 0;
}

static u64
memsize(void *va)
{
  struct Mbmem *e;
  paddr pa = v2p(va);

  e = memsearch(pa);
  if (e == nullptr)
    return -1;
  return (e->base+e->length) - pa;
}

static void *
memnext(void *va, u64 inc)
{
  struct Mbmem *e, *q;
  paddr pa = v2p(va);

  e = memsearch(pa);
  if (e == nullptr)
    return (void *)-1;

  pa += inc;
  if (pa < (e->base+e->length))
    return p2v(pa);

  q = mem+nmem;
  for (e = e + 1; e < q; e++)
      return p2v(e->base);

  return (void *)-1;
}

static void
initmem(u64 mbaddr)
{
  struct Mbdata *mb;
  struct Mbmem *mbmem;
  u8 *p, *ep;

  mb = (Mbdata*) p2v(mbaddr);
  if(!(mb->flags & (1<<6)))
    panic("multiboot header has no memory map");

  p = (u8*) p2v(mb->mmap_addr);
  ep = p + mb->mmap_length;

  while (p < ep) {
    mbmem = (Mbmem *)(p+4);
    p += 4 + *(u32*)p;
    bool use = mbmem->type == 1 && mbmem->base >= 0x100000;
    console.println("e820: ", shex(mbmem->base).width(18).pad(), "-",
                    shex(mbmem->base + mbmem->length - 1).width(18).pad(), " ",
                    use ? "usable" :
                    mbmem->type == 1 ? "usable (ignored)" :
                    "reserved");
    if (use) {
      membytes += mbmem->length;
      if (mbmem->base + mbmem->length > memmax)
        memmax = mbmem->base + mbmem->length;
      mem[nmem] = *mbmem;
      nmem++;
    }
  }
}

// simple page allocator to get off the ground during boot
static char *
pgalloc(void)
{
  if (newend == 0)
    newend = end;

  void *p = (void*)PGROUNDUP((uptr)newend);
  memset(p, 0, PGSIZE);
  newend = newend + PGSIZE;
  return (char*) p;
}

//
// kmem
//
run*
kmem::alloc(const char* name)
{
  run* r;

  for (;;) {
    auto headval = freelist.load();
    r = headval.ptr();
    if (!r)
      return nullptr;
    
    run *nxt = r->next;
    if (freelist.compare_exchange(headval, nxt)) {
      if (r->next != nxt)
        panic("kmem:alloc: aba race %p %p %p\n",
              r, r->next, nxt);
      nfree--;
      if (!name)
        name = this->name;
      mtlabel(mtrace_label_block, r, size, name, strlen(name));
      return r;
    }
  }
}

void
kmem::free(run* r)
{
  if (kinited)
    mtunlabel(mtrace_label_block, r);

  for (;;) {
    auto headval = freelist.load();
    r->next = headval.ptr();
    if (freelist.compare_exchange(headval, r))
      break;
  }
  nfree++;
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
static void
kfree_pool(struct kmem *m, char *v)
{
  if ((uptr)v % PGSIZE) 
    panic("kfree_pool: misaligned %p", v);
  if (memsize(v) == -1ull)
    panic("kfree_pool: unknown region %p", v);

  // Fill with junk to catch dangling refs.
  if (ALLOC_MEMSET && kinited && m->size <= 16384)
    memset(v, 1, m->size);

  m->free((run*)v);
}

static void
kmemprint_pool(const percpu<kmem, percpu_safety::internal> &km)
{
  cprintf("pool %s: [ ", &km[0].name[1]);
  for (u32 i = 0; i < NCPU; i++)
    if (i == mycpu()->id)
      cprintf("<%lu> ", km[i].nfree.load());
    else
      cprintf("%lu ", km[i].nfree.load());
  cprintf("]\n");
}

void
kmemprint()
{
  kmemprint_pool(kmems);
  for (int i = 0; i < slab_type_max; i++)
    kmemprint_pool(slabmem[i]);
}


static char*
kalloc_pool(const percpu<kmem, percpu_safety::internal> &km, const char *name)
{
  struct run *r = 0;
  struct kmem *m;

  u32 startcpu = mycpu()->id;
  for (u32 i = 0; r == 0 && i < NCPU; i++) {
    int cn = (i + startcpu) % NCPU;
    m = &km[cn];
    r = m->alloc(name);
  }

  if (r == 0) {
    cprintf("kalloc: out of memory in pool %s\n", km.get_unchecked()->name);
    // kmemprint();
    return 0;
  }

  if (ALLOC_MEMSET && m->size <= 16384)
    memset(r, 2, m->size);
  return (char*)r;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
char*
kalloc(const char *name)
{
  if (!kinited)
    return pgalloc();
  return kalloc_pool(kmems, name);
}

void *
ksalloc(int slab)
{
  return kalloc_pool(slabmem[slab], nullptr);
}

void
slabinit(struct kmem *k, char **p, u64 *off)
{
  for (int i = 0; i < k->ninit; i++) {
    if (*p == (void *)-1)
      panic("slabinit: memnext");
    while (memsize(*p) < k->size) {
      *p = (char*) memnext(*p, k->size);
      if (*p == (char*)-1)
        panic("slabinit: memsize");
    }
    kfree_pool(k, *p);
    *p = (char*) memnext(*p, k->size);
    *off = *off+k->size;
  }
}  

// Initialize free list of physical pages.
void
initkalloc(u64 mbaddr)
{
  char *p;
  u64 n;
  u64 k;

  initmem(mbaddr);

  // Allocate the page metadata array.  Since there's no point in
  // tracking the pages that store the page metadata array, we compute
  // the optimal size balance so the array starts with the metadata
  // for the page immediately following the array.

  // Translate newend from the small boot mapping at KCODE to the
  // large direct mapping at KBASE.
  page_info_array = (page_info*)((uptr)newend - KCODE + KBASE);
  page_info_len = 1 + (memmax - v2p(newend)) / (sizeof(page_info) + PGSIZE);
  auto page_info_bytes = page_info_len * sizeof(page_info);
  // Find a memory hole large enough to fit page_info_array.
  // XXX(austin) This is really unfortunate on ben, where this forces
  // us to skip the bottom 4 gigs of RAM.  We could break this up into
  // chunks with a fast lookup table.  We could virtually map this
  // (probably with large pages).  If we virtually map it, we might
  // also be able to make it NUMA aware so the metadata for pages is
  // stored on the same physical node as the pages.
  while (memsize(page_info_array) < page_info_bytes)
    page_info_array = (page_info*)memnext(page_info_array, page_info_bytes);
  newend = PGROUNDUP((char*)page_info_array + page_info_bytes);
  page_info_base = v2p(newend);

  for (int c = 0; c < NCPU; c++) {
    kmems[c].name[0] = (char) c + '0';
    safestrcpy(kmems[c].name+1, "kmem", MAXNAME-1);
    kmems[c].size = PGSIZE;
  }

  if (VERBOSE)
    cprintf("%lu mbytes\n", membytes / (1<<20));
  n = (membytes - v2p(newend)) / NCPU;
  if (n & (PGSIZE-1))
    n = PGROUNDDOWN(n);

  p = (char*)PGROUNDUP((uptr)newend);
  k = 0;
  for (int c = 0; c < NCPU; c++) {
    // Fill slab allocators
    strncpy(slabmem[slab_stack][c].name, " kstack", MAXNAME);
    slabmem[slab_stack][c].size = KSTACKSIZE;
    slabmem[slab_stack][c].ninit = CPUKSTACKS;

    strncpy(slabmem[slab_perf][c].name, " kperf", MAXNAME);
    slabmem[slab_perf][c].size = PERFSIZE;
    slabmem[slab_perf][c].ninit = 1;

    strncpy(slabmem[slab_kshared][c].name, " kshared", MAXNAME);
    slabmem[slab_kshared][c].size = KSHAREDSIZE;
    slabmem[slab_kshared][c].ninit = CPUKSTACKS;

    strncpy(slabmem[slab_wq][c].name, " wq", MAXNAME);
    slabmem[slab_wq][c].size = PGROUNDUP(wq_size());
    slabmem[slab_wq][c].ninit = 2;

    strncpy(slabmem[slab_userwq][c].name, " uwq", MAXNAME);
    slabmem[slab_userwq][c].size = USERWQSIZE;
    slabmem[slab_userwq][c].ninit = CPUKSTACKS;

    for (int i = 0; i < slab_type_max; i++) {
      slabmem[i][c].name[0] = (char) c + '0';
      slabinit(&slabmem[i][c], &p, &k);
    }
   
    // The rest goes to the page allocator
    // XXX(austin) This should come from NUMA
    for (; k < n; k += PGSIZE, p = (char*) memnext(p, PGSIZE)) {
      if (p == (void *)-1)
        panic("initkalloc: e820next");
      kfree_pool(&kmems[c], p);
    }

    k = 0;
  }

  kminit();
  kinited = 1;
}

void
kfree(void *v)
{
  kfree_pool(mykmem(), (char*) v);
}

void
ksfree(int slab, void *v)
{
  kfree_pool(&*slabmem[slab], (char*) v);
}
