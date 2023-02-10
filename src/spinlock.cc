#include "spinlock.hh"
#include "common.hh"
#include "spinwait.hh"
#include <atomic>
#include <cstdint>

auto K42Lock::lock() -> void {
  Node node;
  node.state_.store(false, std::memory_order_relaxed);

  auto pre_tail = dummy_.tail_.exchange(&node, std::memory_order_acq_rel);
  if (pre_tail != nullptr) {
    pre_tail->next_.store(&node, std::memory_order_release);
    while (!node.state_.load(std::memory_order_acquire)) {
      cpu_pause();
    }
  }
  auto next = node.next_.load(std::memory_order_acquire);
  if (next == nullptr) {
    dummy_.next_.store(nullptr, std::memory_order_release);
    Node *tmp = &node;
    if (!dummy_.tail_.compare_exchange_strong(tmp, &dummy_,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
      while (nullptr == (next = node.next_.load(std::memory_order_acquire))) {
        cpu_pause();
      }
      dummy_.next_.store(next, std::memory_order_release);
    }
  } else {
    dummy_.next_.store(next, std::memory_order_release);
  }
}

auto K42Lock::unlock() -> void {
  auto next = dummy_.next_.load(std::memory_order_acquire);
  if (next == nullptr) {
    auto tmp = &dummy_;
    if (dummy_.tail_.compare_exchange_strong(tmp, nullptr,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
      return;
    }
    while (nullptr == (next = dummy_.next_.load(std::memory_order_acquire))) {
      cpu_pause();
    }
  }
  next->state_.store(true, std::memory_order_release);
}

auto K42Lock::try_lock() -> bool {
  Node *tmp = nullptr;
  if (dummy_.tail_.compare_exchange_strong(
          tmp, &dummy_, std::memory_order_acq_rel, std::memory_order_relaxed)) {
    return true;
  }
  return false;
}
