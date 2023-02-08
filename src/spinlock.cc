#include "spinlock.hh"

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

HTicketLock::HTicketLock() {
  for (auto &i : numa_arr_) {
    i.store(nullptr, std::memory_order_relaxed);
  }
}

HTicketLock::~HTicketLock() {
  for (auto &i : numa_arr_) {
    auto nnode = i.load(std::memory_order_relaxed);
    if (nnode != nullptr) {
      delete nnode;
    }
  }
}

auto HTicketLock::lock(uint32_t numa_id) -> void {
  auto node = get_or_alloc_nnode(numa_id);
  auto ticket = node->next_ticket_.fetch_add(1, std::memory_order_relaxed);
  while (ticket != node->next_serving_.load(std::memory_order_acquire)) {
    cpu_pause();
  }
  // only one can reach here at same time, g_ticket_ is safe not to be atomic
  if (unlikely(node->g_ticket_ == std::numeric_limits<uint64_t>::max() ||
               node->g_ticket_ !=
                   g_next_serving_.load(std::memory_order_relaxed))) {
    node->g_ticket_ = g_next_ticket_.fetch_add(1, std::memory_order_relaxed);
  }

  while (node->g_ticket_ != g_next_serving_.load(std::memory_order_acquire)) {
    cpu_pause();
  }
  locked_numa_.store(node, std::memory_order_relaxed);
}

auto HTicketLock::unlock() -> void {
  auto node = locked_numa_.load(std::memory_order_relaxed);
  auto c = node->batch_.fetch_add(1, std::memory_order_relaxed);
  if (c < NUMA_BATCH_COUNT &&
      node->next_ticket_.load(std::memory_order_relaxed) !=
          node->next_serving_.load(std::memory_order_relaxed) + 1) {
    node->next_serving_.fetch_add(1, std::memory_order_release);
    return;
  }
  g_next_serving_.fetch_add(1, std::memory_order_release);
  node->next_serving_.fetch_add(1, std::memory_order_release);
}

auto HTicketLock::get_or_alloc_nnode(uint32_t numa_id) -> Node * {
  if ((size_t)numa_id >= MAX_NUMA_NUM) {
    throw std::runtime_error("numa id too large");
  }
  auto &a_ref = numa_arr_[numa_id];
  auto ptr = a_ref.load(std::memory_order_relaxed);
  if (likely(ptr != nullptr)) {
    return ptr;
  }
  // first time
  auto new_node = new Node(numa_id);
  if (a_ref.compare_exchange_strong(ptr, new_node, std::memory_order_acq_rel)) {
    return new_node;
  } else {
    delete new_node;
    return ptr;
  }
}