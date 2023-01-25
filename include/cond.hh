#pragma once

#include "common.hh"
#include "mutex.hh"
#include <abt.h>
#include <cassert>
#include <cstddef>
#include <cstdint>

class Cond : private noncopyable, private nonmoveable {
private:
  struct WaitQ {
    ABT_thread ult_handle_ = ABT_THREAD_NULL;
    WaitQ *next_ = nullptr;
  };

private:
  Mutex q_mu_;
  WaitQ *next_ = nullptr;
  WaitQ *tail_ = nullptr;
  size_t num_waiters_ = 0;
  Mutex *waiter_mu_ = nullptr;

public:
  Cond() = default;
  ~Cond() { assert(num_waiters_ == 0); }

  auto wait(Mutex *mu) -> void;
  auto wait(Mutex *mu, uint32_t numa_id) -> void;
  auto signal_one() -> void;
  auto signal_one(uint32_t numa_id) -> void;
  auto signal_all() -> void;
  auto signal_all(uint32_t numa_id) -> void;
};
