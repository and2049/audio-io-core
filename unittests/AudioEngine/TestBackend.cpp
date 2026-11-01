#include "TestBackend.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <new>
#include <stdexcept>
#include <utility>

#include "Check.h"

namespace {

constexpr uint32_t kDefaultSampleRate = 48000;
constexpr uint32_t kDefaultFrames = 128;

bool supported_rate(uint32_t rate) { return rate == 44100 || rate == 48000 || rate == 96000; }

bool supported_frames(uint32_t frames) { return frames == 64 || frames == 128 || frames == 256; }

uint32_t resolve_channels(const ae::ChannelSelection &selection, uint32_t available) {
  if (selection.mode == AE_CHANNEL_MODE_NONE) {
    return 0;
  }
  if (selection.mode == AE_CHANNEL_MODE_BACKEND_DEFAULT) {
    return available;
  }
  return static_cast<uint32_t>(selection.indices.size());
}

ae_result consumeFailure(TestBackend::Failure &failure) {
  switch (std::exchange(failure, TestBackend::Failure::None)) {
  case TestBackend::Failure::Error:
    return AE_ERROR_DRIVER_FAILURE;
  case TestBackend::Failure::Exception:
    throw std::runtime_error("test backend failure");
  case TestBackend::Failure::Allocation:
    throw std::bad_alloc();
  case TestBackend::Failure::None:
    return AE_OK;
  }
  return AE_ERROR_DRIVER_FAILURE;
}

}

TestBackend::TestBackend() : devices_{{"test", "test-duplex", "Test Duplex", 2, 2}} {}

TestBackend::~TestBackend() {
  stop();
  close();
}

const std::vector<ae::Device> &TestBackend::devices() const { return devices_; }

ae_result TestBackend::open(const ae::OpenRequest &request, ae::ActualFormat &actual) {
  if (request.device_id != "test-duplex") {
    return AE_ERROR_NOT_FOUND;
  }

  uint32_t rate = request.sample_rate;
  if (rate == 0 || !supported_rate(rate)) {
    if (rate != 0 && !request.allow_adjustment) {
      return AE_ERROR_UNSUPPORTED_CONFIGURATION;
    }
    rate = kDefaultSampleRate;
  }
  if (ignore_exact_negotiation_.load(std::memory_order_relaxed)) {
    rate = kDefaultSampleRate;
  }

  uint32_t frames = request.frames_per_block;
  if (frames == 0 || !supported_frames(frames)) {
    if (frames != 0 && !request.allow_adjustment) {
      return AE_ERROR_UNSUPPORTED_CONFIGURATION;
    }
    frames = kDefaultFrames;
  }

  actual.sample_rate = rate;
  actual.frames_per_block = frames;
  actual.max_frames_per_block = frames;
  actual.input_channels = resolve_channels(request.inputs, devices_[0].input_channels);
  actual.output_channels = resolve_channels(request.outputs, devices_[0].output_channels);
  for (uint32_t i = 0; i < actual.input_channels; ++i) {
    actual.input_map.push_back(
        request.inputs.mode == AE_CHANNEL_MODE_EXPLICIT ? request.inputs.indices[i] : i);
  }
  for (uint32_t i = 0; i < actual.output_channels; ++i) {
    actual.output_map.push_back(
        request.outputs.mode == AE_CHANNEL_MODE_EXPLICIT ? request.outputs.indices[i] : i);
  }
  actual_ = actual;
  return AE_OK;
}

ae_result TestBackend::start(ae::BackendProcessFn process, void *engine_state) {
  if (std::exchange(retained_start_failure_, false)) {
    return AE_ERROR_QUIESCENCE_FAILURE;
  }
  if (running_.load(std::memory_order_acquire)) {
    return AE_ERROR_INVALID_STATE;
  }
  if (ae_result result = consumeFailure(start_failure_); result != AE_OK) {
    return result;
  }

  process_ = process;
  engine_state_ = engine_state;
  frame_position_ = 0;
  stop_requested_.store(false, std::memory_order_relaxed);
  running_.store(true, std::memory_order_release);
  try {
    thread_ = std::thread(&TestBackend::run, this);
  } catch (...) {
    running_.store(false, std::memory_order_release);
    process_ = nullptr;
    engine_state_ = nullptr;
    throw;
  }
  return AE_OK;
}

ae_result TestBackend::stop() {
  if (ae_result result = consumeFailure(stop_failure_); result != AE_OK) {
    return result;
  }
  stop_requested_.store(true, std::memory_order_release);
  if (thread_.joinable()) {
    thread_.join();
  }
  running_.store(false, std::memory_order_release);
  process_ = nullptr;
  engine_state_ = nullptr;
  return AE_OK;
}

ae_result TestBackend::close() {
  if (std::exchange(close_failure_, false)) {
    return AE_ERROR_DRIVER_FAILURE;
  }
  actual_ = {};
  faults_.store(0);
  return AE_OK;
}

ae_result TestBackend::probe(const std::string &id, ae::Capabilities &capabilities) {
  if (id != "test-duplex") {
    return AE_ERROR_NOT_FOUND;
  }
  capabilities.info.input_channels = 2;
  capabilities.info.output_channels = 2;
  capabilities.info.current_sample_rate = 48000;
  capabilities.info.min_frames = 64;
  capabilities.info.max_frames = 256;
  capabilities.info.preferred_frames = 128;
  capabilities.info.frame_granularity = -1;
  capabilities.rates = {44100, 48000, 96000};
  capabilities.inputs.resize(2);
  capabilities.outputs.resize(2);
  for (uint32_t i = 0; i < 2; ++i) {
    capabilities.inputs[i].index = i;
    capabilities.outputs[i].index = i;
  }
  return AE_OK;
}

void TestBackend::run() {
  const uint32_t input_channels = actual_.input_channels;
  const uint32_t output_channels = actual_.output_channels;
  const uint32_t frames = actual_.frames_per_block;

  std::vector<float> input(static_cast<size_t>(input_channels) * frames, 0.25f);
  std::vector<float> output(static_cast<size_t>(output_channels) * frames, 0.0f);
  std::vector<const float *> inputs(input_channels);
  std::vector<float *> outputs(output_channels);
  for (uint32_t channel = 0; channel < input_channels; ++channel) {
    inputs[channel] = input.data() + static_cast<size_t>(channel) * frames;
  }
  for (uint32_t channel = 0; channel < output_channels; ++channel) {
    outputs[channel] = output.data() + static_cast<size_t>(channel) * frames;
  }

  while (!stop_requested_.load(std::memory_order_acquire)) {
    std::fill(output.begin(), output.end(), 1.0f);
    ae_process_result result = process_(
        engine_state_, input_channels == 0 ? nullptr : inputs.data(), input_channels,
        output_channels == 0 ? nullptr : outputs.data(), output_channels, frames, frame_position_);
    if (result != AE_PROCESS_OK) {
      AE_CHECK(
          std::all_of(output.begin(), output.end(), [](float sample) { return sample == 0.0f; }));
    }
    frame_position_ += frames;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  running_.store(false, std::memory_order_release);
}
