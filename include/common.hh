#pragma once

#include <abt.h>
#include <cstddef>
#include <stdexcept>

const size_t CACHE_LINE_SIZE = 64;

#define CACHE_LINE_ALIGN __attribute__((aligned(CACHE_LINE_SIZE)))

static inline auto pause() -> void {
#if defined(__i386__) || defined(__x86_64__)
  asm volatile("pause");
#elif defined(__aarch64__)
  asm volatile("isb");
#elif defined(__powerpc64__)
  asm volatile("or 27,27,27");
#elif defined(__loongarch64)
  asm volatile("dbar 0");
#endif
}

static inline auto abt_get_thread(ABT_thread *thread) -> void {
  if (ABT_SUCCESS != ABT_self_get_thread(thread) ||
      *thread == ABT_THREAD_NULL) {
    throw std::runtime_error("failed to get ULT handle, check runtime");
  }
}

static inline auto abt_yield() -> void {
  if (ABT_self_yield() != ABT_SUCCESS) {
    throw std::runtime_error("failed to yield, check runtime");
  }
}

static inline auto abt_suspend() -> void {
  if (ABT_self_suspend() != ABT_SUCCESS) {
    throw std::runtime_error("failed to suspend, check runtime");
  }
}

static inline auto abt_resume(ABT_thread thread) -> void {
  int ret = 0;
  while (ABT_ERR_THREAD == (ret = ABT_thread_resume(thread))) {
    pause();
  }
  if (ret != ABT_SUCCESS) {
    throw std::runtime_error("failed to resume, check runtime");
  }
}

class noncopyable {
public:
  constexpr noncopyable() = default;
  ~noncopyable() = default;
  noncopyable(const noncopyable &) = delete;
  auto operator=(const noncopyable &) -> noncopyable & = delete;
};

class nonmoveable {
public:
  constexpr nonmoveable() = default;
  ~nonmoveable() = default;
  nonmoveable(nonmoveable &&) = delete;
  auto operator=(nonmoveable &&) -> nonmoveable & = delete;
};
