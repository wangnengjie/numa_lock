#include "cond.hh"
#include "common.hh"
#include "mutex.hh"
#include <abt.h>
#include <cassert>
#include <cstdint>
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
  q_mu_.lock(numa_id);
  if (waiter_mu_ == nullptr) {
    waiter_mu_ = mu;
  } else if (waiter_mu_ != mu) {
    throw std::runtime_error("pass different mutex to cond");
  }
  mu->unlock();
  WaitQ node;
  abt_get_thread(&node.ult_handle_);
  // add to queue
  if (next_ == nullptr) {
    assert(num_waiters_ == 0);
    next_ = &node;
  } else {
    tail_->next_ = &node;
  }
  tail_ = &node;
  num_waiters_++;
  q_mu_.unlock();
  abt_suspend();
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
  q_mu_.lock(numa_id);
  if (next_ != nullptr) {
    auto node = next_;
    next_ = node->next_;
    if (next_ == nullptr) {
      waiter_mu_ = nullptr;
      tail_ = nullptr;
    }
    num_waiters_--;
    abt_resume(node->ult_handle_);
  }
  q_mu_.unlock();
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
  q_mu_.lock(numa_id);
  while (next_ != nullptr) {
    auto node = next_;
    next_ = node->next_;
    abt_resume(node->ult_handle_);
  }
  num_waiters_ = 0;
  waiter_mu_ = nullptr;
  tail_ = nullptr;
  q_mu_.unlock();
}
