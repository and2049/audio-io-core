#ifndef AUDIO_ENGINE_SRC_BACKEND_H
#define AUDIO_ENGINE_SRC_BACKEND_H

#include <cstdint>
#include <string>
#include <vector>

#include "audio_engine/engine.h"

namespace ae {

struct Device {
  std::string backend_id;
  std::string device_id;
  std::string display_name;
  uint32_t input_channels = 0;
  uint32_t output_channels = 0;
  bool channel_counts_known = true;
  bool pairable = false;
};

struct Capabilities {
  ae_device_capabilities info{};
  std::vector<uint32_t> rates;
  std::vector<ae_channel_info> inputs;
  std::vector<ae_channel_info> outputs;
};

struct ChannelSelection {
  ae_channel_mode mode = AE_CHANNEL_MODE_NONE;
  std::vector<uint32_t> indices;
};

struct OpenRequest {
  std::string backend_id;
  std::string device_id;
  std::string output_device_id;
  ChannelSelection inputs;
  ChannelSelection outputs;
  uint32_t sample_rate = 0;
  uint32_t frames_per_block = 0;
  bool allow_adjustment = false;
};

struct ActualFormat {
  uint32_t sample_rate = 0;
  uint32_t frames_per_block = 0;
  uint32_t max_frames_per_block = 0;
  uint32_t input_channels = 0;
  uint32_t output_channels = 0;
  std::vector<uint32_t> input_map;
  std::vector<uint32_t> output_map;
};

using BackendProcessFn = ae_process_result (*)(void *engine_state, const float *const *inputs,
                                               uint32_t input_channels, float *const *outputs,
                                               uint32_t output_channels, uint32_t frames,
                                               uint64_t frame_position) noexcept;

class Backend {
public:
  virtual ~Backend() = default;

  virtual const std::vector<Device> &devices() const = 0;
  virtual ae_result refresh() { return AE_OK; }
  virtual ae_result probe(const std::string &, Capabilities &) { return AE_ERROR_UNSUPPORTED; }
  virtual uint32_t faults() const noexcept { return 0; }
  virtual void observations(ae_stream_status &) const noexcept {}
  virtual ae_diagnostic diagnostic() const noexcept { return {}; }
  virtual ae_result open(const OpenRequest &request, ActualFormat &actual) = 0;
  virtual ae_result start(BackendProcessFn process, void *engine_state) = 0;
  virtual ae_result stop() = 0;
  virtual ae_result close() = 0;
};

}

#endif
