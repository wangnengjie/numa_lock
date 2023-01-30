#pragma once

#include "common.hh"
#include <cstddef>
#include <cstdint>
#include <functional>

class SpinWait {
private:
  static const uint32_t SPIN_THRESHOLD = 1024;
  static const uint32_t YILED_THRESHOLD = 8;

private:
  uint32_t counter_{0};
  const uint32_t spin_threshold_;
  const uint32_t yield_threshold_;

public:
  explicit SpinWait(uint32_t spin_threshold = SPIN_THRESHOLD,
                    uint32_t yield_threshold = YILED_THRESHOLD)
      : spin_threshold_(spin_threshold), yield_threshold_(yield_threshold) {}
  auto spin(const std::function<void(void)> &yield,
            const std::function<void(void)> &block) -> void {
    if (counter_ < spin_threshold_) {
      cpu_pause();
    } else if (counter_ < (spin_threshold_ + yield_threshold_)) {
      yield();
    } else {
      block();
    }
    counter_++;
  }
};
