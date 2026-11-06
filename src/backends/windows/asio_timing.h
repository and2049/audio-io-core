#ifndef AUDIO_ENGINE_WINDOWS_ASIO_TIMING_H
#define AUDIO_ENGINE_WINDOWS_ASIO_TIMING_H

#include "callback_timing.h"

namespace ae::detail {
inline uint64_t asio_stall_timeout(uint64_t frequency, uint32_t frames, uint32_t rate) noexcept {
  return stall_timeout(frequency, frames, rate);
}

template <typename Clock>
bool asio_callback_stalled(const std::atomic<uint64_t> &last_callback, uint64_t timeout,
                           Clock clock) noexcept {
  return callback_stalled(last_callback, timeout, clock);
}
}

#endif
