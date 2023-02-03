#pragma once

#include "common.hh"
#include <abt.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

const size_t NUMA_BATCH_COUNT = 128;

enum class NodeState : uint64_t {
  SPIN,
  // ULT suspend, need resume
  SUSPEND,
  // for LocalNode, gain local lock without global
  //
  // for NumaNode, gain global lock
  LOCKED,
  // only LocalNode has this state. both local and global lock was acquired
  LOCKED_WITH_GLOBAL,
};

class CACHE_LINE_ALIGN Mutex : private noncopyable, private nonmoveable {
private:
  friend class RWLock;
  // manual cache line align in lock phase
  struct LocalNode {
    std::atomic<LocalNode *> next_{nullptr};
    // this is only used for dummy in nnode
    std::atomic<LocalNode *> tail_{nullptr};
    std::atomic<NodeState> state_{NodeState::SPIN};
    ABT_thread ult_handle_{ABT_THREAD_NULL};
  };
  struct CACHE_LINE_ALIGN NumaNode {
    LocalNode l_list_;
    std::atomic_uint64_t local_batch_count_{0};
    std::atomic_uint64_t reader_count_{0}; // for RWLock
    std::atomic<NumaNode *> CACHE_LINE_ALIGN next_{nullptr};
    std::atomic<NodeState> state_{NodeState::SPIN};
    ABT_thread ult_handle_{ABT_THREAD_NULL};
  };

private:
  static const size_t MAX_NUMA_NUM =
      CACHE_LINE_SIZE / sizeof(std::atomic<NumaNode *>);

private:
  std::atomic<NumaNode *> locked_numa_{nullptr};
  std::atomic<NumaNode *> ntail_{nullptr};
  // maybe 8 numa is enough in our case?
  std::array<std::atomic<NumaNode *>, MAX_NUMA_NUM> CACHE_LINE_ALIGN numa_arr_;

public:
  Mutex();
  ~Mutex();
  auto lock(uint32_t numa_id = self_numa_id()) -> void;
  auto unlock() -> void;

private:
  auto get_or_alloc_nnode(uint32_t numa_id) -> NumaNode *;
  auto lock_local(NumaNode *nnode) -> NodeState;
  auto lock_global(NumaNode *nnode) -> void;
  auto unlock_local(NumaNode *nnode) -> void;
  auto unlock_global(NumaNode *nnode) -> void;
  auto pass_local_lock(NumaNode *nnode, NodeState state) -> bool;
};
