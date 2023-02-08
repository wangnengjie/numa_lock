#pragma once

#include "common.hh"
#include "mutex.hh"
#include <abt.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

// this is not a numa aware spinlock
class K42Lock : private noncopyable, private nonmoveable {
private:
  struct Node {
    std::atomic<Node *> next_{nullptr};
    union {
      std::atomic<Node *> tail_;
      std::atomic_bool state_;
    };
    Node() {
      next_.store(nullptr, std::memory_order_relaxed);
      tail_.store(nullptr, std::memory_order_relaxed);
    };
  };

private:
  Node dummy_;

public:
  K42Lock() { dummy_.tail_.store(nullptr, std::memory_order_relaxed); }
  auto lock() -> void;
  auto unlock() -> void;
  auto try_lock() -> bool;
};

// hierarchical ticket lock based spinlock
class HTicketLock : private noncopyable, private nonmoveable {
private:
  struct Node {
    std::atomic_uint64_t next_ticket_{0};
    std::atomic_uint64_t next_serving_{0};
    uint64_t g_ticket_{std::numeric_limits<uint64_t>::max()};
    std::atomic_uint64_t batch_{0};
    uint32_t numa_id_;
    Node(uint32_t numa_id) : numa_id_(numa_id) {}
  } CACHE_LINE_ALIGN;

private:
  static const size_t MAX_NUMA_NUM =
      CACHE_LINE_SIZE / sizeof(std::atomic<Node *>);
  static const uint64_t L_BIT = 0b001;
  static const uint64_t G_BIT = 0b010;

private:
  std::atomic_uint64_t g_next_ticket_{0};
  std::atomic_uint64_t g_next_serving_{0};
  std::atomic<Node *> locked_numa_{nullptr};
  std::array<std::atomic<Node *>, MAX_NUMA_NUM> CACHE_LINE_ALIGN numa_arr_;

public:
  HTicketLock();
  ~HTicketLock();
  auto lock(uint32_t numa_id = self_numa_id()) -> void;
  auto unlock() -> void;
  auto get_or_alloc_nnode(uint32_t numa_id) -> Node *;
};

// classic TTAS lock
class TTASLock : private noncopyable, private nonmoveable {
private:
  std::atomic_flag state_{false};

public:
  auto lock() -> void {
    while (state_.test_and_set(std::memory_order_acquire)) {
      while (state_.test(std::memory_order_relaxed)) {
        cpu_pause();
      }
    }
  }

  auto unlock() -> void { state_.clear(std::memory_order_release); }

  auto try_lock() -> bool {
    return !state_.test_and_set(std::memory_order_acquire);
  }
};

class CLHLock : private noncopyable, private nonmoveable {
public:
  struct QNode {
    friend class CLHLock;

  private:
    QNode *prev_{nullptr};
    std::atomic_bool state_{false};

  public:
    QNode(bool lock = false) : state_(lock) {}
  };

private:
  std::atomic<QNode *> tail_{new QNode(true)};

public:
  CLHLock() = default;
  ~CLHLock() { delete tail_.load(std::memory_order_relaxed); }
  auto lock(QNode *my) -> void {
    my->state_.store(false, std::memory_order_relaxed);
    my->prev_ = tail_.exchange(my, std::memory_order_acquire);
    while (!my->prev_->state_.load(std::memory_order_acquire)) {
      cpu_pause();
    }
  }
  auto unlock(QNode **my) -> void {
    auto prev = (*my)->prev_;
    (*my)->state_.store(true, std::memory_order_release);
    *my = prev;
  }
};