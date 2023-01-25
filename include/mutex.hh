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
  // for SocketNode, gain global lock
  LOCKED,
  // only LocalNode has this state. both local and global lock was acquired
  LOCKED_WITH_GLOBAL,
};

class CACHE_LINE_ALIGN Mutex {

private:
  struct LocalNode;
  struct SocketNode;

private:
  static const size_t MAX_SOCKET_NUM =
      CACHE_LINE_SIZE / sizeof(std::atomic<SocketNode *>);

private:
  std::atomic<SocketNode *> locked_socket;
  std::atomic<SocketNode *> stail;
  // maybe 8 socket is enough in our case?
  std::array<std::atomic<SocketNode *>, MAX_SOCKET_NUM> CACHE_LINE_ALIGN
      socket_arr;

public:
  Mutex();
  ~Mutex();
  auto lock() -> void;
  auto lock(uint32_t numa_id) -> void;
  auto unlock() -> void;

private:
  auto get_or_alloc_snode(uint32_t numa_id) -> SocketNode *;
  auto lock_local(SocketNode *snode) -> NodeState;
  auto lock_global(SocketNode *snode) -> void;
  auto unlock_local(SocketNode *snode) -> void;
  auto unlock_global(SocketNode *snode) -> void;
  auto pass_local_lock(SocketNode *snode, NodeState state) -> bool;
};

// manual cache line align in lock phase
struct Mutex::LocalNode {
  std::atomic<LocalNode *> next;
  // this is only used for dummy in snode
  std::atomic<LocalNode *> tail;
  std::atomic<NodeState> state;
  ABT_thread ult_handle;
  LocalNode(bool init_ult_handle = true)
      : next(nullptr), tail(nullptr), state(NodeState::INIT) {
    if (init_ult_handle && (ABT_SUCCESS != ABT_self_get_thread(&ult_handle) ||
                            ult_handle == ABT_THREAD_NULL)) {
      throw std::runtime_error("failed to get ULT handle, check runtime");
    }
  }
};

struct CACHE_LINE_ALIGN Mutex::SocketNode {
  // local access part
  LocalNode l_list;
  std::atomic_uint64_t local_batch_count;
  // global access part
  std::atomic<SocketNode *> CACHE_LINE_ALIGN next;
  std::atomic<NodeState> state;
  ABT_thread ult_handle;
  SocketNode()
      : l_list(false), local_batch_count(0), next(nullptr),
        state(NodeState::INIT), ult_handle(ABT_THREAD_NULL) {}
};
