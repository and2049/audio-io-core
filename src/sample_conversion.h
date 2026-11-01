#ifndef AUDIO_ENGINE_SAMPLE_CONVERSION_H
#define AUDIO_ENGINE_SAMPLE_CONVERSION_H

#include <cstdint>

namespace ae {
struct SampleFormat {
  uint32_t bytes = 0;
  bool floating = false;
  bool big_endian = false;
};

bool valid_format(SampleFormat format) noexcept;
void decode_samples(SampleFormat format, const void *source, float *destination,
                    uint32_t frames) noexcept;
void encode_samples(SampleFormat format, const float *source, void *destination,
                    uint32_t frames) noexcept;
}

#endif
