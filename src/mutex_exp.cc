#include "mutex_exp.hh"
#include "common.hh"
#include "mutex.hh"
#include "spinwait.hh"
#include <abt.h>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>

namespace experimental {

auto Mutex::get_or_alloc_nnode(uint32_t numa_id) -> NNode * {
  if ((size_t)numa_id >= MAX_NUMA_NUM) {
    throw std::runtime_error("numa id too large");
  }
  auto &a_ref = numa_arr_[numa_id];
  auto ptr = a_ref.load(std::memory_order_relaxed);
  if (likely(ptr != nullptr)) {
    return ptr;
  }
  // first time
  auto new_node = new NNode(numa_id);
  if (a_ref.compare_exchange_strong(ptr, new_node)) {
    return new_node;
  } else {
    delete new_node;
    return ptr;
  }
}

auto Mutex::lock(QNode *my, uint32_t numa_id) -> void {
  auto nnode = get_or_alloc_nnode(numa_id);
  auto state = lock_local(nnode, my);
  if (state == LOCK) {
    lock_global(nnode);
  } else {
    assert(state == G_LOCK);
  }
}

auto Mutex::lock_local(NNode *nnode, QNode *my) -> State {
  my->ult_handle_ = ABT_THREAD_NULL;
  my->numa_id_ = nnode->numa_id_;
  my->has_next_.store(false, std::memory_order_relaxed);
  my->state_.store(SPIN, std::memory_order_relaxed);

  my->prev_ = nnode->qtail_.exchange(my, std::memory_order_acq_rel);
  abt_get_thread(&my->prev_->ult_handle_);
  State s;
  while (SPIN == (s = my->prev_->state_.load(std::memory_order_acquire))) {
    if (my->prev_->state_.compare_exchange_weak(
            s, SUSPEND, std::memory_order_acq_rel, std::memory_order_relaxed)) {
      abt_suspend();
    }
  }
  assert(s == LOCK || s == G_LOCK);
  return s;
}

auto Mutex::lock_global(NNode *nnode) -> void {
  abt_get_thread(&nnode->ult_handle_);
  nnode->nnext_.store(nullptr, std::memory_order_relaxed);
  nnode->state_.store(SPIN, std::memory_order_relaxed);

  auto prev = ntail_.exchange(nnode, std::memory_order_acq_rel);
  if (prev == nullptr) {
    nnode->state_.store(LOCK, std::memory_order_release);
    return;
  }
  prev->nnext_.store(nnode, std::memory_order_release);
  SpinWait sw;
  State s;
  while (SPIN == (s = nnode->state_.load(std::memory_order_acquire))) {
    sw.spin(abt_yield, [&nnode, &s]() {
      if (nnode->state_.compare_exchange_weak(s, SUSPEND,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
        abt_suspend();
      }
    });
  }
  assert(s == LOCK);
}

auto Mutex::unlock(QNode **my) -> void {
  // after lock, my's numa_id must be set to a valid value
  auto nnode = numa_arr_[(*my)->numa_id_].load(std::memory_order_relaxed);
  auto c = nnode->local_batch_count_.fetch_add(1, std::memory_order_relaxed);
  // weak check has next
  if (c < NUMA_BATCH_COUNT && (*my)->ult_handle_ != ABT_THREAD_NULL) {
    unlock_local(my, G_LOCK);
    return;
  }
  nnode->local_batch_count_.store(0, std::memory_order_relaxed);
  // unlock global
  unlock_global(nnode);
  // unlock_local(nnode, my);
  unlock_local(my, LOCK);
}

auto Mutex::unlock_local(QNode **my, State state) -> void {
  auto prev = (*my)->prev_;
  State s = (*my)->state_.exchange(state, std::memory_order_acq_rel);
  if (s == SUSPEND) {
    abt_resume((*my)->ult_handle_);
  } else {
    assert(s == SPIN);
  }
  *my = prev;
}

auto Mutex::unlock_global(NNode *nnode) -> void {
  auto next = nnode->nnext_.load(std::memory_order_acquire);
  if (next == nullptr) {
    auto tmp = nnode;
    if (ntail_.compare_exchange_strong(tmp, nullptr, std::memory_order_acq_rel,
                                       std::memory_order_relaxed)) {
      return;
    }
    // some global waiter add to list, wait next update
    while (nullptr == (next = nnode->nnext_.load(std::memory_order_acquire))) {
      cpu_pause();
    }
  }
  State s = next->state_.exchange(LOCK, std::memory_order_acq_rel);
  if (s == SUSPEND) {
    abt_resume(next->ult_handle_);
  } else {
    assert(s == SPIN);
  }
}

} // namespace experimental