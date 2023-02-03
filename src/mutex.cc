#include "mutex.hh"
#include "common.hh"
#include "spinwait.hh"
#include <abt.h>
#include <atomic>
#include <cstddef>
#include <stdexcept>

Mutex::Mutex() {
  for (auto &i : numa_arr_) {
    i.store(nullptr, std::memory_order_relaxed);
  }
}

Mutex::~Mutex() {
  for (auto &i : numa_arr_) {
    auto nnode = i.load(std::memory_order_relaxed);
    if (nnode != nullptr) {
      delete nnode;
    }
  }
}

auto Mutex::get_or_alloc_nnode(uint32_t numa_id) -> NumaNode * {
  if ((size_t)numa_id >= MAX_NUMA_NUM) {
    throw std::runtime_error("numa id too large");
  }
  auto &a_ref = numa_arr_[numa_id];
  auto ptr = a_ref.load(std::memory_order_relaxed);
  if (likely(ptr != nullptr)) {
    return ptr;
  }
  // first time
  auto new_node = new NumaNode();
  if (a_ref.compare_exchange_strong(ptr, new_node, std::memory_order_acq_rel)) {
    return new_node;
  } else {
    delete new_node;
    return ptr;
  }
}

auto Mutex::lock(uint32_t numa_id) -> void {
  auto nnode = get_or_alloc_nnode(numa_id);
  auto state = lock_local(nnode);
  switch (state) {
  case NodeState::LOCKED:
    lock_global(nnode);
    break;
  case NodeState::LOCKED_WITH_GLOBAL:
    break;
  default:
    throw std::runtime_error("invalid state");
  }
}

auto Mutex::lock_local(NumaNode *nnode) -> NodeState {
  LocalNode lnode CACHE_LINE_ALIGN; // cache line align
  abt_get_thread(&lnode.ult_handle_);

  LocalNode *dummy = &nnode->l_list_;
  LocalNode *pre_tail =
      dummy->tail_.exchange(&lnode, std::memory_order_acq_rel);

  if (pre_tail == nullptr) {
    lnode.state_.store(NodeState::LOCKED, std::memory_order_relaxed);
  } else if (pre_tail != nullptr) {
    pre_tail->next_.store(&lnode, std::memory_order_release);
    SpinWait spinwait;
    while (lnode.state_.load(std::memory_order_relaxed) == NodeState::SPIN) {
      spinwait.spin(abt_yield, [&lnode]() {
        // we need to suspend
        auto tmp = NodeState::SPIN;
        if (lnode.state_.compare_exchange_weak(tmp, NodeState::SUSPEND,
                                               std::memory_order_acq_rel,
                                               std::memory_order_relaxed)) {
          abt_suspend();
          // after suspend, we get the lock and will break loop
        }
      });
    }
  }
  // we get local lock
  auto next_lnode = lnode.next_.load(std::memory_order_acquire);
  if (next_lnode == nullptr) {
    dummy->next_.store(nullptr, std::memory_order_release);
    //! no next node, try to set tail to dummy which indicate local is locked
    auto expected = &lnode;
    if (!dummy->tail_.compare_exchange_strong(expected, dummy,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
      // dummy->tail is not cur_node, other thread append to list
      // wait lnode next to be setted, avoid segment fault
      while (nullptr ==
             (next_lnode = lnode.next_.load(std::memory_order_acquire))) {
        cpu_pause();
      }
      dummy->next_.store(next_lnode, std::memory_order_release);
    }
  } else {
    dummy->next_.store(next_lnode, std::memory_order_release);
  }
  // relax is okay here
  return lnode.state_.load(std::memory_order_relaxed);
}

auto Mutex::lock_global(NumaNode *nnode) -> void {
  // we need to add nnode to global list
  // we check runtime in local lock, no need to check again?
  abt_get_thread(&nnode->ult_handle_);
  nnode->next_.store(nullptr, std::memory_order_relaxed);
  nnode->state_.store(NodeState::SPIN, std::memory_order_relaxed);

  auto pre_tail = ntail_.exchange(nnode, std::memory_order_acq_rel);
  if (pre_tail == nullptr) {
    nnode->state_.store(NodeState::LOCKED, std::memory_order_relaxed);
    locked_numa_.store(nnode, std::memory_order_release);
    return;
  }
  pre_tail->next_.store(nnode, std::memory_order_release);
  SpinWait spinwait;
  while (nnode->state_.load(std::memory_order_relaxed) == NodeState::SPIN) {
    spinwait.spin(abt_yield, [&nnode]() {
      // we need to suspend
      auto state = NodeState::SPIN;
      if (nnode->state_.compare_exchange_weak(state, NodeState::SUSPEND,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
        abt_suspend();
        // after suspend, we get the lock and will break loop
      }
    });
  }
  // get global lock
  locked_numa_.store(nnode, std::memory_order_release);
  return;
}

auto Mutex::unlock() -> void {
  auto nnode = locked_numa_.load(std::memory_order_acquire);
  // caller should protect the mutex is locked
  auto c = nnode->local_batch_count_.fetch_add(1, std::memory_order_relaxed);
  if (c < NUMA_BATCH_COUNT) {
    if (pass_local_lock(nnode, NodeState::LOCKED_WITH_GLOBAL)) {
      return;
    }
    // pass false, no local waiter
  }
  nnode->local_batch_count_.store(0, std::memory_order_relaxed);
  unlock_global(nnode);
  unlock_local(nnode);
  return;
}

auto Mutex::pass_local_lock(NumaNode *nnode, NodeState state) -> bool {
  LocalNode *lnode = nnode->l_list_.next_.load(std::memory_order_acquire);
  if (lnode == nullptr) {
    return false;
  }
  NodeState pre_state =
      lnode->state_.exchange(state, std::memory_order_acq_rel);
  switch (pre_state) {
  case NodeState::SPIN:
    break;
  case NodeState::SUSPEND:
    abt_resume(lnode->ult_handle_);
    break;
  default:
    throw std::runtime_error("invalid state");
    break;
  }
  return true;
}

auto Mutex::unlock_global(NumaNode *nnode) -> void {
  NumaNode *next = nnode->next_.load(std::memory_order_acquire);
  if (next == nullptr) {
    auto tmp = nnode;
    if (ntail_.compare_exchange_strong(tmp, nullptr, std::memory_order_acq_rel,
                                       std::memory_order_relaxed)) {
      return;
    }
    // some global waiter add to list, wait next update
    while (nullptr == (next = nnode->next_.load(std::memory_order_acquire))) {
      cpu_pause();
    }
  }
  NodeState pre_state =
      next->state_.exchange(NodeState::LOCKED, std::memory_order_acq_rel);
  switch (pre_state) {
  case NodeState::SPIN:
    break;
  case NodeState::SUSPEND:
    abt_resume(next->ult_handle_);
    break;
  default:
    throw std::runtime_error("invalid state");
    break;
  }
}

auto Mutex::unlock_local(NumaNode *nnode) -> void {
  if (nnode->l_list_.next_.load(std::memory_order_acquire) == nullptr) {
    auto tmp = &nnode->l_list_;
    if (nnode->l_list_.tail_.compare_exchange_strong(
            tmp, nullptr, std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
      return;
    }
    while (nnode->l_list_.next_.load(std::memory_order_acquire) == nullptr) {
      cpu_pause();
    }
  }
  pass_local_lock(nnode, NodeState::LOCKED);
}
