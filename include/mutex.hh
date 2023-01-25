#pragma once

#include "common.hh"
#include <abt.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

const size_t SPIN_THRESHOLD = 0;
const size_t YILED_SPIN_THRESHOLD = 5;
const size_t NUMA_BATCH_COUNT = 64;

enum class NodeState : uint64_t {
  INIT,
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

class Mutex {
private:
  struct LocalNode;
  struct NumaNode;

private:
  static const size_t MAX_NUMA_NUM =
      CACHE_LINE_SIZE / sizeof(std::atomic<NumaNode *>);

private:
  std::atomic<NumaNode *> locked_numa_;
  std::atomic<NumaNode *> ntail_;
  // maybe 8 numa is enough in our case?
  std::array<std::atomic<NumaNode *>, MAX_NUMA_NUM> CACHE_LINE_ALIGN numa_arr;

public:
  Mutex();
  ~Mutex();
  auto lock() -> void;
  auto lock(uint32_t numa_id) -> void;
  auto unlock() -> void;

private:
  auto get_or_alloc_nnode(uint32_t numa_id) -> NumaNode *;
  auto lock_local(NumaNode *nnode) -> NodeState;
  auto lock_global(NumaNode *nnode) -> void;
  auto unlock_local(NumaNode *nnode) -> void;
  auto unlock_global(NumaNode *nnode) -> void;
  auto pass_local_lock(NumaNode *nnode, NodeState state) -> bool;
};

// manual cache line align in lock phase
struct Mutex::LocalNode {
  std::atomic<LocalNode *> next_;
  // this is only used for dummy in nnode
  std::atomic<LocalNode *> tail_;
  std::atomic<NodeState> state_;
  ABT_thread ult_handle_;
  LocalNode(bool init_ult_handle = true)
      : next_(nullptr), tail_(nullptr), state_(NodeState::INIT) {
    if (init_ult_handle && (ABT_SUCCESS != ABT_self_get_thread(&ult_handle_) ||
                            ult_handle_ == ABT_THREAD_NULL)) {
      throw std::runtime_error("failed to get ULT handle, check runtime");
    }
  }
};

struct CACHE_LINE_ALIGN Mutex::NumaNode {
  // local access part
  LocalNode l_list_;
  std::atomic_uint64_t local_batch_count_;
  // global access part
  std::atomic<NumaNode *> CACHE_LINE_ALIGN next_;
  std::atomic<NodeState> state_;
  ABT_thread ult_handle_;
  NumaNode()
      : l_list_(false), local_batch_count_(0), next_(nullptr),
        state_(NodeState::INIT), ult_handle_(ABT_THREAD_NULL) {}
};
