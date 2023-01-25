#include "cond.hh"
#include "mutex.hh"
#include <abt.h>
#include <cassert>
#include <cstdint>
#include <emmintrin.h>
#include <iostream>
#include <stdexcept>

auto Cond::wait(Mutex *mu) -> void {
  uint32_t cpu_id, numa_id;
  int ret = getcpu(&cpu_id, &numa_id);
  if (ret != 0) {
    throw std::runtime_error("failed to get numa id");
  }
  return wait(mu, numa_id);
}

auto Cond::wait(Mutex *mu, uint32_t numa_id) -> void {
  q_mu.lock(numa_id);
  if (waiter_mu == nullptr) {
    waiter_mu = mu;
  } else if (waiter_mu != mu) {
    throw std::runtime_error("pass different mutex to cond");
  }
  mu->unlock();
  WaitQ node;
  if (ABT_SUCCESS != ABT_self_get_thread(&node.ult_handle) ||
      node.ult_handle == ABT_THREAD_NULL) {
    throw std::runtime_error("failed to get ULT handle, check runtime");
  }
  // add to queue
  if (next == nullptr) {
    assert(num_waiters == 0);
    next = &node;
  } else {
    tail->next = &node;
  }
  tail = &node;
  num_waiters++;
  q_mu.unlock();
  if (ABT_SUCCESS != ABT_self_suspend()) {
    throw std::runtime_error("failed to suspend, check runtime");
  }
  // resumed
  mu->lock(numa_id);
}

auto Cond::signal_one() -> void {
  uint32_t cpu_id, numa_id;
  int ret = getcpu(&cpu_id, &numa_id);
  if (ret != 0) {
    throw std::runtime_error("failed to get numa id");
  }
  return signal_one(numa_id);
}

auto Cond::signal_one(uint32_t numa_id) -> void {
  q_mu.lock(numa_id);
  if (next != nullptr) {
    auto node = next;
    next = node->next;
    if (next == nullptr) {
      waiter_mu = nullptr;
      tail = nullptr;
    }
    num_waiters--;
    while (ABT_ERR_THREAD == ABT_thread_resume(node->ult_handle)) {
      _mm_pause();
    }
  }
  q_mu.unlock();
}

auto Cond::signal_all() -> void {
  uint32_t cpu_id, numa_id;
  int ret = getcpu(&cpu_id, &numa_id);
  if (ret != 0) {
    throw std::runtime_error("failed to get numa id");
  }
  return signal_all(numa_id);
}

auto Cond::signal_all(uint32_t numa_id) -> void {
  q_mu.lock(numa_id);
  while (next != nullptr) {
    auto node = next;
    next = node->next;
    if (next == nullptr) {
      waiter_mu = nullptr;
      tail = nullptr;
    }
    num_waiters--;
    while (ABT_ERR_THREAD == ABT_thread_resume(node->ult_handle)) {
      _mm_pause();
    }
  }
  q_mu.unlock();
}
