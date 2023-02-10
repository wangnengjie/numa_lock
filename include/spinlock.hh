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
class HTTAS : private noncopyable, private nonmoveable {
  friend class RWLock;

private:
  struct Node {
    std::atomic_uint64_t flag_{0};
    std::atomic_uint64_t CACHE_LINE_ALIGN reader_count_{0};
    std::atomic_uint64_t waiter_count_{0};
    uint32_t batch_count_{0};
    auto reader_count() -> uint64_t {
      return reader_count_.load(std::memory_order_acquire);
    }
    auto incr_reader() -> uint64_t {
      return reader_count_.fetch_add(1, std::memory_order_release);
    }
    auto decr_reader() -> uint64_t {
      return reader_count_.fetch_sub(1, std::memory_order_release);
    }
  } CACHE_LINE_ALIGN;

private:
  static const size_t MAX_NUMA_NUM =
      CACHE_LINE_SIZE / sizeof(std::atomic<Node *>);

private:
  std::atomic_uint64_t flag_{0};
  std::array<std::atomic<Node *>, MAX_NUMA_NUM> CACHE_LINE_ALIGN numa_arr_;

public:
  HTTAS() {
    for (auto &i : numa_arr_) {
      i.store(nullptr, std::memory_order_relaxed);
    }
  }
  ~HTTAS() {
    for (auto &i : numa_arr_) {
      auto nnode = i.load(std::memory_order_relaxed);
      if (nnode != nullptr) {
        delete nnode;
      }
    }
  }

  auto lock(uint32_t numa_id = self_numa_id()) -> void {
    auto nnode = get_or_alloc_nnode(numa_id);
    nnode->waiter_count_.fetch_add(1, std::memory_order_relaxed);
    uint8_t s = 0;
    while (1 == (s = nnode->flag_.exchange(1, std::memory_order_acquire))) {
      while (nnode->flag_.load(std::memory_order_relaxed) == 1) {
        abt_yield();
      }
    }
    nnode->waiter_count_.fetch_sub(1, std::memory_order_relaxed);
    if (s == 0) {
      while (flag_.exchange(1, std::memory_order_acquire)) {
        while (flag_.load(std::memory_order_relaxed)) {
          // cpu_pause();
          abt_yield();
        }
      }
    }
  }

  auto unlock(uint32_t numa_id = self_numa_id()) -> void {
    auto nnode = get_or_alloc_nnode(numa_id);
    auto c = nnode->batch_count_++;
    if (c < NUMA_BATCH_COUNT &&
        nnode->waiter_count_.load(std::memory_order_relaxed)) {
      nnode->flag_.store(2, std::memory_order_release);
      return;
    }
    nnode->batch_count_ = 0;
    flag_.store(0, std::memory_order_release);
    nnode->flag_.store(0, std::memory_order_release);
  }

  auto is_locked() -> bool { return flag_.load(std::memory_order_relaxed); }

private:
  auto get_or_alloc_nnode(uint32_t numa_id) -> Node * {
    if ((size_t)numa_id >= MAX_NUMA_NUM) {
      throw std::runtime_error("numa id too large");
    }
    auto &a_ref = numa_arr_[numa_id];
    auto ptr = a_ref.load(std::memory_order_relaxed);
    if (likely(ptr != nullptr)) {
      return ptr;
    }
    // first time
    auto new_node = new Node();
    if (a_ref.compare_exchange_strong(ptr, new_node,
                                      std::memory_order_acq_rel)) {
      return new_node;
    } else {
      delete new_node;
      return ptr;
    }
  }
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