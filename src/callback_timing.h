#ifndef AUDIO_ENGINE_SRC_CALLBACK_TIMING_H
#define AUDIO_ENGINE_SRC_CALLBACK_TIMING_H

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace ae::detail {
inline uint64_t stall_timeout(uint64_t frequency, uint32_t frames, uint32_t rate) noexcept {
  return std::max(frequency * 2, frequency * frames / rate * 3);
}

template <typename Clock>
bool callback_stalled(const std::atomic<uint64_t> &last_callback, uint64_t timeout,
                      Clock clock) noexcept {
  const uint64_t last = last_callback.load();
  const uint64_t current = clock();
  return current >= last && current - last > timeout && last_callback.load() == last;
}
}

#endif
