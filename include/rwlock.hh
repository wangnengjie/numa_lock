#pragma once

#include "common.hh"
#include "spinlock.hh"
#include "spinwait.hh"
#include <atomic>
#include <cassert>
#include <cstdint>

class RWLock : private noncopyable, private nonmoveable {
private:
  HTTAS lock_;

public:
  auto rdlock(uint32_t numa_id = self_numa_id()) -> void;
  auto rdunlock(uint32_t numa_id = self_numa_id()) -> void;
  auto wrlock(uint32_t numa_id = self_numa_id()) -> void;
  auto wrunlock(uint32_t numa_id = self_numa_id()) -> void;
};

inline auto RWLock::rdlock(uint32_t numa_id) -> void {
  auto nnode = lock_.get_or_alloc_nnode(numa_id);
  SpinWait sw;
  while (true) {
    while (lock_.is_locked()) {
      sw.spin_no_block(abt_yield);
    }
    nnode->incr_reader();
    // need recheck
    if (lock_.is_locked()) {
      nnode->decr_reader();
      sw.reset();
      continue;
    }
    break;
  }
}

inline auto RWLock::rdunlock(uint32_t numa_id) -> void {
  auto nnode = lock_.get_or_alloc_nnode(numa_id);
  auto prev = nnode->decr_reader();
  assert(prev != 0);
}

inline auto RWLock::wrlock(uint32_t numa_id) -> void {
  SpinWait sw;
  lock_.lock(numa_id);
  // no reader can get rlock after we lock
  for (auto &p : lock_.numa_arr_) {
    sw.reset();
    auto nnode = p.load(std::memory_order_relaxed);
    if (nnode != nullptr) {
      while (nnode->reader_count() != 0) {
        sw.spin_no_block(abt_yield);
      }
    }
  }
}

inline auto RWLock::wrunlock(uint32_t numa_id) -> void {
  lock_.unlock(numa_id);
}
