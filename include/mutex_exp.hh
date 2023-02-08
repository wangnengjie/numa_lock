#pragma once

#include "common.hh"
#include <abt.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>

namespace experimental {

class CACHE_LINE_ALIGN Mutex : private noncopyable, private nonmoveable {
private:
  enum State : uint8_t {
    SPIN,
    SUSPEND,
    LOCK,
    G_LOCK,
  };

public:
  struct QNode {
    friend class Mutex;

  private:
    std::atomic<State> state_{SPIN};
    std::atomic_bool has_next_{false};
    uint32_t numa_id_{std::numeric_limits<uint32_t>::max()};
    QNode *prev_{nullptr};
    ABT_thread ult_handle_{ABT_THREAD_NULL};

  public:
    explicit QNode(State state = SPIN) : state_(state) {}
  };

  struct NNode {
    friend class Mutex;

  private:
    const uint32_t numa_id_;
    std::atomic<QNode *> qtail_{new QNode(LOCK)};
    std::atomic_uint8_t local_batch_count_{0};
    std::atomic<NNode *> nnext_{nullptr};
    std::atomic<State> state_{SPIN};
    ABT_thread ult_handle_{ABT_THREAD_NULL};

  public:
    explicit NNode(uint32_t numa_id) : numa_id_(numa_id) {}
    ~NNode() { delete qtail_.load(std::memory_order_relaxed); }
  };

public:
private:
  // maybe not enough
  static const size_t MAX_NUMA_NUM =
      CACHE_LINE_SIZE / sizeof(std::atomic<NNode *>);

  std::atomic<NNode *> ntail_{nullptr};
  std::array<std::atomic<NNode *>, MAX_NUMA_NUM> CACHE_LINE_ALIGN numa_arr_;

public:
  Mutex() {
    for (auto &ptr : numa_arr_) {
      ptr.store(nullptr, std::memory_order_relaxed);
    }
  }
  ~Mutex() {
    for (auto &ptr : numa_arr_) {
      auto nnode = ptr.load(std::memory_order_relaxed);
      if (nnode != nullptr) {
        delete nnode;
      }
    }
  }
  auto lock(QNode *my, uint32_t numa_id = self_numa_id()) -> void;
  auto unlock(QNode **my) -> void;

private:
  auto get_or_alloc_nnode(uint32_t numa_id) -> NNode *;
  auto lock_local(NNode *nnode, QNode *my) -> State;
  auto lock_global(NNode *nnode) -> void;
  // return true if success pass G_LOCK to succ
  auto unlock_local(QNode **my, State state) -> void;
  auto unlock_global(NNode *nnode) -> void;
};

} // namespace experimental