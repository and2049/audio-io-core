#include <array>
#include <bit>
#include <cmath>
#include <limits>

#include "Check.h"
#include "sample_conversion.h"

namespace {
void float64_boundaries() {
  const double maximum = std::numeric_limits<float>::max();
  const double infinity = std::numeric_limits<double>::infinity();
  const double values[] = {0.0,
                           -0.0,
                           0.25,
                           -0.25,
                           2.0,
                           -2.0,
                           maximum,
                           -maximum,
                           std::nextafter(maximum, 0.0),
                           std::nextafter(-maximum, 0.0),
                           std::nextafter(maximum, infinity),
                           std::nextafter(-maximum, -infinity),
                           1.0e40,
                           -1.0e40,
                           std::numeric_limits<double>::max(),
                           -std::numeric_limits<double>::max(),
                           infinity,
                           -infinity,
                           std::numeric_limits<double>::quiet_NaN()};
  for (bool big : {false, true}) {
    for (double value : values) {
      const uint64_t raw = std::bit_cast<uint64_t>(value);
      std::array<uint8_t, 10> bytes;
      bytes.fill(0xcd);
      for (uint32_t byte = 0; byte < 8; ++byte) {
        bytes[1 + (big ? 7 - byte : byte)] = static_cast<uint8_t>(raw >> (8 * byte));
      }
      std::array<float, 3> decoded{42.0f, 42.0f, 42.0f};
      ae::decode_samples({8, true, big}, bytes.data() + 1, decoded.data() + 1, 1);
      AE_CHECK(decoded.front() == 42.0f && decoded.back() == 42.0f);
      AE_CHECK(bytes.front() == 0xcd && bytes.back() == 0xcd);
      if (!std::isfinite(value) || std::abs(value) > maximum) {
        AE_CHECK(decoded[1] == 0.0f);
      } else {
        AE_CHECK(decoded[1] == static_cast<float>(value));
        AE_CHECK(std::signbit(decoded[1]) == std::signbit(value));
      }
    }
  }
}
}

int main() {
  float64_boundaries();
  AE_CHECK(!ae::valid_format({1, false, false}));
  AE_CHECK(!ae::valid_format({3, true, false}));
  const float input[] = {-2.0f,
                         -1.0f,
                         -0.5f,
                         0.0f,
                         0.5f,
                         1.0f,
                         2.0f,
                         std::numeric_limits<float>::infinity(),
                         std::numeric_limits<float>::quiet_NaN()};
  for (bool big : {false, true}) {
    for (uint32_t width : {2u, 3u, 4u}) {
      const ae::SampleFormat format{width, false, big};
      std::array<uint8_t, 40> bytes;
      bytes.fill(0xcd);
      std::array<float, 9> decoded{};
      ae::encode_samples(format, input, bytes.data() + 1, 9);
      AE_CHECK(bytes[0] == 0xcd && bytes[1 + 9 * width] == 0xcd);
      AE_CHECK(bytes[big ? 1 : width] == 0x80);
      ae::decode_samples(format, bytes.data() + 1, decoded.data(), 9);
      AE_CHECK(decoded[0] == -1.0f && decoded[1] == -1.0f);
      AE_CHECK(decoded[2] == -0.5f && decoded[3] == 0.0f && decoded[4] == 0.5f);
      AE_CHECK(decoded[5] > 0.99f && decoded[5] <= 1.0f && decoded[6] == decoded[5]);
      AE_CHECK(decoded[7] == 0.0f && decoded[8] == 0.0f);
      ae::decode_samples(format, nullptr, nullptr, 0);
      ae::encode_samples(format, nullptr, nullptr, 0);
    }
    for (uint32_t width : {4u, 8u}) {
      const ae::SampleFormat format{width, true, big};
      std::array<uint8_t, 74> bytes;
      bytes.fill(0xcd);
      std::array<float, 9> decoded{};
      ae::encode_samples(format, input, bytes.data() + 1, 9);
      ae::decode_samples(format, bytes.data() + 1, decoded.data(), 9);
      AE_CHECK(bytes[0] == 0xcd && bytes[1 + 9 * width] == 0xcd);
      for (uint32_t i = 0; i < 7; ++i) {
        AE_CHECK(decoded[i] == input[i]);
      }
      AE_CHECK(decoded[7] == 0.0f && decoded[8] == 0.0f);
    }
  }
  const uint8_t packed[] = {0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0xff, 0xff, 0x7f};
  float decoded[3]{};
  ae::decode_samples({3, false, false}, packed, decoded, 3);
  AE_CHECK(decoded[0] == -1.0f / 8388608.0f);
  AE_CHECK(decoded[1] == 1.0f / 8388608.0f);
  AE_CHECK(decoded[2] == 8388607.0f / 8388608.0f);
}
