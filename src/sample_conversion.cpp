#include "sample_conversion.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>

namespace ae {
namespace {
uint64_t read_sample(const uint8_t *source, SampleFormat format) noexcept {
  uint64_t value = 0;
  for (uint32_t i = 0; i < format.bytes; ++i) {
    const uint32_t byte = format.big_endian ? format.bytes - 1 - i : i;
    value |= static_cast<uint64_t>(source[byte]) << (i * 8);
  }
  return value;
}

void write_sample(uint8_t *destination, uint64_t value, SampleFormat format) noexcept {
  for (uint32_t i = 0; i < format.bytes; ++i) {
    const uint32_t byte = format.big_endian ? format.bytes - 1 - i : i;
    destination[byte] = static_cast<uint8_t>(value >> (i * 8));
  }
}
}

bool valid_format(SampleFormat format) noexcept {
  return format.floating ? format.bytes == 4 || format.bytes == 8
                         : format.bytes >= 2 && format.bytes <= 4;
}

void decode_samples(SampleFormat format, const void *source, float *destination,
                    uint32_t frames) noexcept {
  const auto *bytes = static_cast<const uint8_t *>(source);
  const double scale = std::ldexp(1.0, static_cast<int>(format.bytes * 8 - 1));
  for (uint32_t i = 0; i < frames; ++i) {
    const uint64_t raw = read_sample(bytes + static_cast<size_t>(i) * format.bytes, format);
    float value;
    if (format.floating) {
      if (format.bytes == 4) {
        value = std::bit_cast<float>(static_cast<uint32_t>(raw));
      } else {
        const double sample = std::bit_cast<double>(raw);
        value = std::isfinite(sample) && std::abs(sample) <= std::numeric_limits<float>::max()
                    ? static_cast<float>(sample)
                    : 0.0f;
      }
    } else {
      const uint64_t sign = uint64_t{1} << (format.bytes * 8 - 1);
      const int64_t signed_value = (raw & sign) != 0
                                       ? static_cast<int64_t>(raw) - static_cast<int64_t>(sign * 2)
                                       : static_cast<int64_t>(raw);
      value = static_cast<float>(static_cast<double>(signed_value) / scale);
    }
    destination[i] = std::isfinite(value) ? value : 0.0f;
  }
}

void encode_samples(SampleFormat format, const float *source, void *destination,
                    uint32_t frames) noexcept {
  auto *bytes = static_cast<uint8_t *>(destination);
  const double scale = std::ldexp(1.0, static_cast<int>(format.bytes * 8 - 1));
  for (uint32_t i = 0; i < frames; ++i) {
    const float value = std::isfinite(source[i]) ? source[i] : 0.0f;
    uint64_t raw;
    if (format.floating) {
      raw = format.bytes == 4 ? std::bit_cast<uint32_t>(value)
                              : std::bit_cast<uint64_t>(static_cast<double>(value));
    } else {
      const double sample =
          std::clamp(std::round(static_cast<double>(value) * scale), -scale, scale - 1.0);
      raw = static_cast<uint64_t>(static_cast<int64_t>(sample));
    }
    write_sample(bytes + static_cast<size_t>(i) * format.bytes, raw, format);
  }
}
}
