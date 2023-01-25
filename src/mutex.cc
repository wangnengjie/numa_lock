#include "mutex.hh"
#include <abt.h>
#include <atomic>
#include <cstddef>
#include <emmintrin.h>
#include <stdexcept>

Mutex::Mutex() : locked_socket(nullptr), stail(nullptr) {
  for (auto &i : socket_arr) {
    i = nullptr;
  }
}

Mutex::~Mutex() {
  for (auto &i : socket_arr) {
    auto snode = i.load(std::memory_order_acquire);
    if (snode != nullptr) {
      delete snode;
    }
  }
}

auto Mutex::get_or_alloc_snode(uint32_t numa_id) -> SocketNode * {
  if ((size_t)numa_id >= MAX_SOCKET_NUM) {
    throw std::runtime_error("numa id too large");
  }
  auto &a_ref = socket_arr[numa_id];
  auto ptr = a_ref.load(std::memory_order_acquire);
  if (ptr != nullptr) {
    return ptr;
  }
  // first time
  auto new_node = new SocketNode();
  if (a_ref.compare_exchange_strong(ptr, new_node)) {
    return new_node;
  } else {
    delete new_node;
    return ptr;
  }
}

auto Mutex::lock() -> void {
  uint32_t cpu_id, numa_id;
  int ret = getcpu(&cpu_id, &numa_id);
  if (ret != 0) {
    throw std::runtime_error("failed to get numa id");
  }
  lock(numa_id);
}

auto Mutex::lock(uint32_t numa_id) -> void {
  auto snode = get_or_alloc_snode(numa_id);
  auto state = lock_local(snode);
  switch (state) {
  case NodeState::LOCKED:
    lock_global(snode);
    break;
  case NodeState::LOCKED_WITH_GLOBAL:
    break;
  default:
    throw std::runtime_error("invalid state");
  }
}

auto Mutex::lock_local(SocketNode *snode) -> NodeState {
  LocalNode lnode CACHE_LINE_ALIGN; // cache line align
  lnode.state.store(NodeState::SPIN, std::memory_order_release);
  LocalNode *dummy = &snode->l_list;

  LocalNode *pre_tail = dummy->tail.exchange(&lnode);

  if (pre_tail == nullptr) {
    lnode.state.store(NodeState::LOCKED, std::memory_order_release);
  } else if (pre_tail != nullptr) {
    pre_tail->next.store(&lnode, std::memory_order_release);

    for (size_t i = 0;
         lnode.state.load(std::memory_order_acquire) == NodeState::SPIN;) {
      if (i < SPIN_THRESHOLD) {
        _mm_pause();
        i++;
      } else if (i < SPIN_THRESHOLD + YILED_SPIN_THRESHOLD) {
        int ret = ABT_self_yield();
        if (ret != ABT_SUCCESS) {
          throw std::runtime_error("failed to yield, check runtime");
        }
        i++;
      } else {
        // we need to suspend
        auto state = NodeState::SPIN;
        if (lnode.state.compare_exchange_weak(state, NodeState::SUSPEND,
                                              std::memory_order_seq_cst,
                                              std::memory_order_relaxed)) {
          int ret = ABT_self_suspend();
          if (ret != ABT_SUCCESS) {
            throw std::runtime_error("failed to suspend, check runtime");
          }
          // after suspend, we get the lock and will break loop
        }
      }
    }
  }
  // we get local lock
  auto next_lnode = lnode.next.load(std::memory_order_acquire);
  dummy->next.store(next_lnode, std::memory_order_release);
  if (next_lnode == nullptr) {
    // no next node, try to set tail to dummy which indicate local is locked
    auto expected = &lnode;
    if (!dummy->tail.compare_exchange_strong(expected, dummy,
                                             std::memory_order_seq_cst,
                                             std::memory_order_relaxed)) {
      // dummy->tail is not cur_node, other thread append to list
      // wait lnode next to be setted, avoid segment fault
      while (lnode.next.load(std::memory_order_acquire) == nullptr) {
        _mm_pause();
      }
      // update snode->lnext, relaxed order is okay
      dummy->next.store(lnode.next.load(std::memory_order_relaxed));
    }
  }
  // relax is okay here
  return lnode.state.load(std::memory_order_relaxed);
}

auto Mutex::lock_global(SocketNode *snode) -> void {
  // we need to add snode to global list
  // we check runtime in local lock, no need to check again?
  ABT_self_get_thread(&snode->ult_handle);
  snode->local_batch_count.store(0, std::memory_order_release);
  snode->next.store(nullptr, std::memory_order_release);
  snode->state.store(NodeState::SPIN, std::memory_order_release);

  auto pre_tail = stail.exchange(snode);
  if (pre_tail == nullptr) {
    snode->state.store(NodeState::LOCKED, std::memory_order_release);
    locked_socket.store(snode, std::memory_order_release);
    return;
  }
  pre_tail->next.store(snode, std::memory_order_release);

  for (size_t i = 0;
       snode->state.load(std::memory_order_acquire) == NodeState::SPIN;) {
    if (i < SPIN_THRESHOLD) {
      _mm_pause();
      i++;
    } else if (i < SPIN_THRESHOLD + YILED_SPIN_THRESHOLD) {
      int ret = ABT_self_yield();
      if (ret != ABT_SUCCESS) {
        throw std::runtime_error("failed to yield, check runtime");
      }
      i++;
    } else {
      // we need to suspend
      auto state = NodeState::SPIN;
      if (snode->state.compare_exchange_weak(state, NodeState::SUSPEND,
                                             std::memory_order_seq_cst,
                                             std::memory_order_relaxed)) {
        int ret = ABT_self_suspend();
        if (ret != ABT_SUCCESS) {
          throw std::runtime_error("failed to suspend, check runtime");
        }
        // after suspend, we get the lock and will break loop
      }
    }
  }
  // get global lock
  locked_socket.store(snode, std::memory_order_release);
  return;
}

auto Mutex::unlock() -> void {
  auto snode = locked_socket.load(std::memory_order_acquire);
  // caller should protect the mutex is locked
  auto c = snode->local_batch_count++;
  if (c < NUMA_BATCH_COUNT) {
    if (pass_local_lock(snode, NodeState::LOCKED_WITH_GLOBAL)) {
      return;
    }
    // pass false, no local waiter
  }
  snode->local_batch_count = 0;
  unlock_global(snode);
  unlock_local(snode);
  return;
}

auto Mutex::pass_local_lock(SocketNode *snode, NodeState state) -> bool {
  LocalNode *lnode = snode->l_list.next.load(std::memory_order_acquire);
  if (lnode == nullptr) {
    return false;
  }
  NodeState pre_state = lnode->state.exchange(state);
  switch (pre_state) {
  case NodeState::SPIN:
    break;
  case NodeState::SUSPEND:
    while (ABT_ERR_THREAD == ABT_thread_resume(lnode->ult_handle)) {
      _mm_pause();
    }
    break;
  default:
    throw std::runtime_error("invalid state");
    break;
  }
  return true;
}

auto Mutex::unlock_global(SocketNode *snode) -> void {
  SocketNode *next = snode->next.load(std::memory_order_acquire);
  if (next == nullptr) {
    auto tmp = snode;
    if (stail.compare_exchange_strong(tmp, nullptr, std::memory_order_seq_cst,
                                      std::memory_order_relaxed)) {
      return;
    }
    // some global waiter add to list, wait next update
    do {
      next = snode->next.load(std::memory_order_acquire);
      _mm_pause();
    } while (next == nullptr);
  }
  NodeState pre_state = next->state.exchange(NodeState::LOCKED);
  switch (pre_state) {
  case NodeState::SPIN:
    break;
  case NodeState::SUSPEND:
    while (ABT_ERR_THREAD == ABT_thread_resume(next->ult_handle)) {
      _mm_pause();
    }
    break;
  default:
    throw std::runtime_error("invalid state");
    break;
  }
}

auto Mutex::unlock_local(SocketNode *snode) -> void {
  if (snode->l_list.next.load(std::memory_order_acquire) == nullptr) {
    auto tmp = &snode->l_list;
    if (snode->l_list.tail.compare_exchange_strong(tmp, nullptr,
                                                   std::memory_order_seq_cst,
                                                   std::memory_order_relaxed)) {
      return;
    }
    while (snode->l_list.next.load(std::memory_order_acquire) == nullptr) {
      _mm_pause();
    }
  }
  pass_local_lock(snode, NodeState::LOCKED);
}