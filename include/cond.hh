#pragma once

#include "mutex.hh"
#include <abt.h>
#include <cassert>
#include <cstddef>
#include <cstdint>
class Cond {
private:
  struct WaitQ {
    ABT_thread ult_handle;
    WaitQ *next;
    WaitQ() : ult_handle(ABT_THREAD_NULL), next(nullptr) {}
  };

private:
  Mutex q_mu;
  WaitQ *next;
  WaitQ *tail;
  size_t num_waiters;
  Mutex *waiter_mu;

public:
  Cond() : next(nullptr), tail(nullptr), num_waiters(0), waiter_mu(nullptr) {}
  ~Cond() { assert(num_waiters == 0); }

  auto wait(Mutex *mu) -> void;
  auto wait(Mutex *mu, uint32_t numa_id) -> void;
  auto signal_one() -> void;
  auto signal_one(uint32_t numa_id) -> void;
  auto signal_all() -> void;
  auto signal_all(uint32_t numa_id) -> void;
};
