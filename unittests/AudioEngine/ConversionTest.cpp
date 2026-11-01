#include <array>
#include <cmath>
#include <limits>

#include "Check.h"
#include "sample_conversion.h"

int main() {
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
