#pragma once

#include <abt.h>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <syscall.h>
#include <unistd.h>

static inline auto barrier() -> void { asm volatile("" : : : "memory"); }

#if defined(__x86_64__)
const size_t CACHE_LINE_SIZE = 128;
static inline auto rmb() -> void { asm volatile("lfence" : : : "memory"); }
static inline auto wmb() -> void { asm volatile("sfence" : : : "memory"); }
static inline auto mb() -> void { asm volatile("mfence" : : : "memory"); }
static inline auto cpu_relax() -> void {
  asm volatile("pause\n" : : : "memory");
}
static inline auto cpu_pause() -> void { asm volatile("pause"); }
#elif defined(__arm64__) || defined(__aarch64__)
const size_t CACHE_LINE_SIZE = 128;
static inline auto rmb() -> void { asm volatile("dmb ishld" : : : "memory"); }
static inline auto wmb() -> void { asm volatile("dmb ishst" : : : "memory"); }
static inline auto mb() -> void { asm volatile("dmb ish" : : : "memory"); }
static inline auto cpu_relax() -> void { asm volatile("yield" : : : "memory"); }
static inline auto cpu_pause() -> void { asm volatile("isb"); }
#else
#error "unsupported arch"
#endif

#define CACHE_LINE_ALIGN __attribute__((aligned(CACHE_LINE_SIZE)))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

static inline auto abt_get_thread(ABT_thread *thread) -> void {
  if (unlikely(ABT_SUCCESS != ABT_self_get_thread(thread) ||
               *thread == ABT_THREAD_NULL)) {
    throw std::runtime_error("failed to get ULT handle, check runtime");
  }
}

static inline auto abt_yield() -> void {
  if (unlikely(ABT_self_yield() != ABT_SUCCESS)) {
    throw std::runtime_error("failed to yield, check runtime");
  }
}

static inline auto abt_suspend() -> void {
  if (unlikely(ABT_self_suspend() != ABT_SUCCESS)) {
    throw std::runtime_error("failed to suspend, check runtime");
  }
}

static inline auto abt_resume(ABT_thread thread) -> void {
  int ret = 0;
  while (ABT_ERR_THREAD == (ret = ABT_thread_resume(thread))) {
    cpu_pause();
  }
  if (unlikely(ret != ABT_SUCCESS)) {
    throw std::runtime_error("failed to resume, check runtime");
  }
}

static inline auto get_cpu_numa(uint32_t *cpu_id, uint32_t *numa_id) -> void {
  int ret = syscall(__NR_getcpu, cpu_id, numa_id);
  // int ret = getcpu(&cpu_id, &numa_id);
  if (unlikely(ret != 0)) {
    throw std::runtime_error("failed to get numa id");
  }
}

static inline auto self_cpu_id() -> uint32_t {
  uint32_t cpu_id;
  get_cpu_numa(&cpu_id, nullptr);
  return cpu_id;
}

static inline auto self_numa_id() -> uint32_t {
  uint32_t numa_id;
  get_cpu_numa(nullptr, &numa_id);
  return numa_id;
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
