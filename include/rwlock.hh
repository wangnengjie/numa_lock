#pragma once

#include "common.hh"
#include "mutex.hh"
#include "spinwait.hh"
#include <atomic>
#include <cassert>
#include <cstdint>

class RWLock : private noncopyable, private nonmoveable {
private:
  Mutex lock_;

public:
  auto rdlock(uint32_t numa_id = self_numa_id()) -> void;
  auto rdunlock(uint32_t numa_id = self_numa_id()) -> void;
  auto wrlock(uint32_t numa_id = self_numa_id()) -> void;
  auto wrunlock() -> void;
};

inline auto RWLock::rdlock(uint32_t numa_id) -> void {
  auto nnode = lock_.get_or_alloc_nnode(numa_id);
  SpinWait sw;
  while (true) {
    while (lock_.ntail_.load(std::memory_order_acquire) != nullptr) {
      sw.spin(abt_yield, abt_yield);
    }
    nnode->reader_count_.fetch_add(1, std::memory_order_release);
    // need recheck
    if (lock_.ntail_.load(std::memory_order_acquire) != nullptr) {
      nnode->reader_count_.fetch_sub(1, std::memory_order_release);
      sw.reset();
      continue;
    }
    break;
  }
}

inline auto RWLock::rdunlock(uint32_t numa_id) -> void {
  auto nnode = lock_.get_or_alloc_nnode(numa_id);
  auto prev = nnode->reader_count_.fetch_sub(1, std::memory_order_release);
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
      while (nnode->reader_count_.load(std::memory_order_acquire) != 0) {
        sw.spin(abt_yield, abt_yield);
      }
    }
  }
}

inline auto RWLock::wrunlock() -> void { lock_.unlock(); }
