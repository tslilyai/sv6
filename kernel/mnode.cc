#include "types.h"
#include "kernel.hh"
#include "mnode.hh"
#include "weakcache.hh"
#include "atomic_util.hh"
#include "percpu.hh"

namespace {
  // 32MB icache (XXX make this proportional to physical RAM)
  weakcache<u64, mnode, 32 << 20> mnode_cache;
};

sref<mnode>
mfs::get(u64 inum)
{
  for (;;) {
    sref<mnode> m = mnode_cache.lookup(inum);
    if (m) {
      // wait for the mnode to be loaded from disk
      while (!m->valid_) {
        /* spin */
      }
      return m;
    }

    panic("read in from disk not implemented");
  }
}

mlinkref
mfs::alloc(u8 type)
{
  scoped_cli cli;
  auto inum = mnode::inumber(type, myid(), (*next_inum_)++).v_;

  sref<mnode> m;
  switch (type) {
  case mnode::types::dir:
    m = sref<mnode>::transfer(new mdir(this, inum));
    break;

  case mnode::types::file:
    m = sref<mnode>::transfer(new mfile(this, inum));
    break;

  case mnode::types::dev:
    m = sref<mnode>::transfer(new mdev(this, inum));
    break;

  case mnode::types::sock:
    m = sref<mnode>::transfer(new msock(this, inum));
    break;

  default:
    panic("unknown type in inum 0x%lx", inum);
  }

  if (!mnode_cache.insert(inum, m.get()))
    panic("mnode_cache insert failed (duplicate inumber?)");

  m->cache_pin(true);
  m->valid_ = true;
  mlinkref mlink(std::move(m));
  mlink.transfer();
  return mlink;
}

mnode::mnode(mfs* fs, u64 inum)
  : fs_(fs), inum_(inum), cache_pin_(false), valid_(false)
{
  kstats::inc(&kstats::mnode_alloc);
}

void
mnode::cache_pin(bool flag)
{
  if (cache_pin_ == flag || !cmpxch(&cache_pin_, !flag, flag))
    return;

  if (flag)
    inc();
  else
    dec();
}

void
mnode::onzero()
{
  mnode_cache.cleanup(weakref_);
  kstats::inc(&kstats::mnode_free);
  delete this;
}

void
mnode::linkcount::onzero()
{
  /*
   * This might fire several times, because the link count of a zero-nlink
   * parent directory can be temporarily revived by mkdir (see create).
   */
  mnode* m = container_from_member(this, &mnode::nlink_);
  m->cache_pin(false);
}

void
mfile::resizer::resize_nogrow(u64 newsize)
{
  u64 oldsize = mf_->size_;
  mf_->size_ = newsize;
  assert(PGROUNDUP(newsize) <= PGROUNDUP(oldsize));
  auto begin = mf_->pages_.find(PGROUNDUP(newsize) / PGSIZE);
  auto end = mf_->pages_.find(PGROUNDUP(oldsize) / PGSIZE);
  auto lock = mf_->pages_.acquire(begin, end);
  mf_->pages_.unset(begin, end);
}

void
mfile::resizer::resize_append(u64 size, sref<page_info> pi)
{
  assert(PGROUNDUP(mf_->size_) / PGSIZE + 1 == PGROUNDUP(size) / PGSIZE);
  auto it = mf_->pages_.find(PGROUNDUP(mf_->size_) / PGSIZE);
  auto lock = mf_->pages_.acquire(it);
  mf_->pages_.fill(it, page_state(pi));
  mf_->size_ = size;
}

sref<page_info>
mfile::get_page(u64 pageidx)
{
  auto it = pages_.find(pageidx);
  if (!it.is_set()) {
    if (pageidx < PGROUNDUP(size_) / PGSIZE) {
      // XXX read from disk
    }

    return sref<page_info>();
  }

  /*
   * Ensure the page_info object is not garbage-collected by refcache,
   * by preventing the local core from going through a refcache epoch.
   * Here, we assume that all stores to sref::ptr_ are atomic, and we
   * will either see a valid pointer or nullptr.
   */
  scoped_cli cli;
  page_info* pi = it->pg.get();
  return sref<page_info>::newref(pi);
}

void
mfsprint(print_stream *s)
{
  auto stats = mnode_cache.get_stats();
  s->println("mnode cache:");
  s->println("  ", stats.items, " items");
  s->println("  ", stats.used_buckets, " used / ",
             stats.total_buckets, " total buckets (",
             stats.used_buckets * 100 / stats.total_buckets, "%)");
  s->println("  ", stats.max_chain, " max chain length");
  s->println("  ", stats.items / stats.total_buckets, " avg chain length");
  if (stats.used_buckets)
    s->println("  ", stats.items / stats.used_buckets, " avg used chain length");
}
