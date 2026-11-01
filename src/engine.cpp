#include "audio_engine/engine.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "backend.h"
#include "testing_api.h"

struct ae_device_snapshot {
  ae_context *context = nullptr;
  std::vector<std::string> strings;
  std::vector<ae_device_info> infos;
};

struct ae_capability_snapshot {
  ae_context *context = nullptr;
  ae::Capabilities capabilities;
};

struct ae_stream {
  ae_context *context = nullptr;
  ae::Backend *backend = nullptr;
  ae_stream_format format{};
  std::atomic<ae_stream_state> state{AE_STREAM_STATE_READY};
  std::atomic<uint32_t> run_generation{0};
  std::atomic<uint64_t> delivered_frames{0};
  std::atomic<ae_result> last_result{AE_OK};
  std::atomic<bool> failure_latched{false};
  ae_process_fn process = nullptr;
  void *user = nullptr;
  std::vector<uint32_t> inputs;
  std::vector<uint32_t> outputs;
};

struct ae_context {
  std::unique_ptr<ae::Backend> backend;
  uint32_t next_stream_id = 1;
  uint32_t next_configuration_generation = 1;
  uint32_t active_streams = 0;
  uint32_t snapshots = 0;
};

namespace {

constexpr uint32_t kMinSampleRate = 8000;

template <typename Function> ae_result call(Function &&function) noexcept {
  try {
    return function();
  } catch (const std::bad_alloc &) {
    return AE_ERROR_ALLOCATION_FAILURE;
  } catch (...) {
    return AE_ERROR_DRIVER_FAILURE;
  }
}

template <typename T> bool has_required_size(const T *value) {
  return value != nullptr && static_cast<size_t>(value->struct_size) >= sizeof(T);
}

bool non_empty(const char *text) { return text != nullptr && text[0] != '\0'; }

bool valid_selection(const ae_channel_selection &selection, uint32_t available_channels) {
  switch (selection.mode) {
  case AE_CHANNEL_MODE_NONE:
  case AE_CHANNEL_MODE_BACKEND_DEFAULT:
    return selection.count == 0;
  case AE_CHANNEL_MODE_EXPLICIT:
    if (selection.count == 0 || selection.indices == nullptr ||
        selection.count > available_channels) {
      return false;
    }
    for (uint32_t i = 0; i < selection.count; ++i) {
      if (selection.indices[i] >= available_channels) {
        return false;
      }
      for (uint32_t j = 0; j < i; ++j) {
        if (selection.indices[j] == selection.indices[i]) {
          return false;
        }
      }
    }
    return true;
  default:
    return false;
  }
}

bool expected_channel_count(const ae_channel_selection &selection, uint32_t available_channels,
                            uint32_t actual_channels) {
  switch (selection.mode) {
  case AE_CHANNEL_MODE_NONE:
    return actual_channels == 0;
  case AE_CHANNEL_MODE_BACKEND_DEFAULT:
    return actual_channels > 0 && actual_channels <= available_channels;
  case AE_CHANNEL_MODE_EXPLICIT:
    return actual_channels == selection.count;
  default:
    return false;
  }
}

std::vector<uint32_t> copy_indices(const ae_channel_selection &selection) {
  if (selection.count == 0) {
    return {};
  }
  return std::vector<uint32_t>(selection.indices, selection.indices + selection.count);
}

const ae::Device *find_device(const ae_context *context, const char *backend_id,
                              const char *device_id) {
  for (const ae::Device &device : context->backend->devices()) {
    if (device.backend_id == backend_id && device.device_id == device_id) {
      return &device;
    }
  }
  return nullptr;
}

class EmptyBackend final : public ae::Backend {
public:
  const std::vector<ae::Device> &devices() const override { return devices_; }
  ae_result open(const ae::OpenRequest &, ae::ActualFormat &) override {
    return AE_ERROR_NOT_FOUND;
  }
  ae_result start(ae::BackendProcessFn, void *) override { return AE_ERROR_INVALID_STATE; }
  ae_result stop() override { return AE_OK; }
  ae_result close() override { return AE_OK; }

private:
  std::vector<ae::Device> devices_;
};

ae_process_result dispatch(void *state, const float *const *inputs, uint32_t input_channels,
                           float *const *outputs, uint32_t output_channels, uint32_t frames,
                           uint64_t frame_position) noexcept {
  auto *stream = static_cast<ae_stream *>(state);
  auto silence = [&] {
    for (uint32_t channel = 0; channel < output_channels; ++channel) {
      std::fill_n(outputs[channel], frames, 0.0f);
    }
  };
  silence();
  if (stream->failure_latched.load(std::memory_order_acquire) || stream->backend->faults() != 0) {
    return AE_PROCESS_SILENCE;
  }

  ae_audio_block block{};
  block.struct_size = sizeof(ae_audio_block);
  block.inputs = inputs;
  block.input_channels = input_channels;
  block.outputs = outputs;
  block.output_channels = output_channels;
  block.frames = frames;
  block.stream_id = stream->format.stream_id;
  block.configuration_generation = stream->format.configuration_generation;
  block.run_generation = stream->run_generation.load(std::memory_order_relaxed);
  block.frame_position = frame_position;

  ae_process_result result;
  try {
    result = stream->process(stream->user, &block);
  } catch (...) {
    result = AE_PROCESS_FAILED;
  }
  if (stream->backend->faults() != 0) {
    silence();
    return AE_PROCESS_SILENCE;
  }
  if (result != AE_PROCESS_OK) {
    silence();
  }
  if (result != AE_PROCESS_OK && result != AE_PROCESS_SILENCE) {
    stream->last_result.store(AE_ERROR_PROCESSOR_FAILURE, std::memory_order_relaxed);
    stream->failure_latched.store(true, std::memory_order_release);
    stream->state.store(AE_STREAM_STATE_RECOVERY_REQUIRED, std::memory_order_release);
    return AE_PROCESS_FAILED;
  }

  stream->delivered_frames.fetch_add(frames, std::memory_order_relaxed);
  return result;
}

ae_result enumerate_devices(const ae_context *context, ae_device_snapshot **out_snapshot) {
  if (context == nullptr || out_snapshot == nullptr) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  *out_snapshot = nullptr;

  const std::vector<ae::Device> &devices = context->backend->devices();
  auto snapshot = std::make_unique<ae_device_snapshot>();
  snapshot->context = const_cast<ae_context *>(context);
  snapshot->strings.reserve(devices.size() * 3);
  for (const ae::Device &device : devices) {
    snapshot->strings.push_back(device.backend_id);
    snapshot->strings.push_back(device.device_id);
    snapshot->strings.push_back(device.display_name);
  }

  snapshot->infos.reserve(devices.size());
  size_t string_index = 0;
  for (const ae::Device &device : devices) {
    ae_device_info info{};
    info.struct_size = sizeof(ae_device_info);
    info.backend_id = snapshot->strings[string_index].c_str();
    info.device_id = snapshot->strings[string_index + 1].c_str();
    info.display_name = snapshot->strings[string_index + 2].c_str();
    info.input_channels = device.input_channels;
    info.output_channels = device.output_channels;
    info.channel_counts_known = device.channel_counts_known;
    snapshot->infos.push_back(info);
    string_index += 3;
  }

  ++const_cast<ae_context *>(context)->snapshots;
  *out_snapshot = snapshot.release();
  return AE_OK;
}

ae_result open_stream(ae_context *context, const ae_stream_request *request, ae_stream **out_stream,
                      ae_stream_format *out_format) {
  if (context == nullptr || request == nullptr || out_stream == nullptr || out_format == nullptr) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  *out_stream = nullptr;
  if (!has_required_size(request) || !has_required_size(out_format)) {
    return AE_ERROR_INCOMPATIBLE_ABI;
  }
  if (!non_empty(request->backend_id) || !non_empty(request->input_device_id) ||
      !non_empty(request->output_device_id)) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  if (request->negotiation != AE_NEGOTIATION_EXACT &&
      request->negotiation != AE_NEGOTIATION_ALLOW_ADJUSTMENT) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  if (request->sample_rate != 0 &&
      (request->sample_rate < kMinSampleRate || request->sample_rate > AE_MAX_SAMPLE_RATE)) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  if (request->frames_per_block > AE_MAX_FRAMES_PER_BLOCK) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  if (std::strcmp(request->input_device_id, request->output_device_id) != 0) {
    return AE_ERROR_UNSUPPORTED_CONFIGURATION;
  }

  const ae::Device *device = find_device(context, request->backend_id, request->input_device_id);
  if (device == nullptr) {
    return AE_ERROR_NOT_FOUND;
  }
  const uint32_t available_inputs =
      device->channel_counts_known ? device->input_channels : AE_MAX_CHANNELS;
  const uint32_t available_outputs =
      device->channel_counts_known ? device->output_channels : AE_MAX_CHANNELS;
  if (!valid_selection(request->input_channels, available_inputs) ||
      !valid_selection(request->output_channels, available_outputs)) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  if (request->input_channels.mode == AE_CHANNEL_MODE_NONE &&
      request->output_channels.mode == AE_CHANNEL_MODE_NONE) {
    return AE_ERROR_UNSUPPORTED_CONFIGURATION;
  }
  if ((request->input_channels.mode != AE_CHANNEL_MODE_NONE && available_inputs == 0) ||
      (request->output_channels.mode != AE_CHANNEL_MODE_NONE && available_outputs == 0)) {
    return AE_ERROR_UNSUPPORTED_CONFIGURATION;
  }
  if (context->active_streams != 0) {
    return AE_ERROR_BUSY;
  }

  ae::OpenRequest spec;
  spec.backend_id = request->backend_id;
  spec.device_id = request->input_device_id;
  spec.inputs.mode = request->input_channels.mode;
  spec.inputs.indices = copy_indices(request->input_channels);
  spec.outputs.mode = request->output_channels.mode;
  spec.outputs.indices = copy_indices(request->output_channels);
  spec.sample_rate = request->sample_rate;
  spec.frames_per_block = request->frames_per_block;
  spec.allow_adjustment = request->negotiation == AE_NEGOTIATION_ALLOW_ADJUSTMENT;

  ae::ActualFormat actual{};
  ae_result result = context->backend->open(spec, actual);
  if (result != AE_OK) {
    return result;
  }

  auto reject = [&](ae_result code) {
    const ae_result cleanup = context->backend->close();
    return cleanup == AE_OK ? code : cleanup;
  };

  if (actual.sample_rate < kMinSampleRate || actual.sample_rate > AE_MAX_SAMPLE_RATE ||
      actual.frames_per_block == 0 || actual.max_frames_per_block < actual.frames_per_block ||
      actual.frames_per_block > AE_MAX_FRAMES_PER_BLOCK ||
      actual.max_frames_per_block > AE_MAX_FRAMES_PER_BLOCK) {
    return reject(AE_ERROR_DRIVER_FAILURE);
  }
  if (!expected_channel_count(request->input_channels, available_inputs, actual.input_channels) ||
      !expected_channel_count(request->output_channels, available_outputs,
                              actual.output_channels)) {
    return reject(AE_ERROR_UNSUPPORTED_CONFIGURATION);
  }
  if (request->negotiation == AE_NEGOTIATION_EXACT) {
    if ((request->sample_rate != 0 && actual.sample_rate != request->sample_rate) ||
        (request->frames_per_block != 0 && actual.frames_per_block != request->frames_per_block)) {
      return reject(AE_ERROR_UNSUPPORTED_CONFIGURATION);
    }
  }

  if (actual.input_map.size() != actual.input_channels ||
      actual.output_map.size() != actual.output_channels) {
    return reject(AE_ERROR_DRIVER_FAILURE);
  }

  auto *stream = new (std::nothrow) ae_stream();
  if (stream == nullptr) {
    return reject(AE_ERROR_ALLOCATION_FAILURE);
  }
  stream->context = context;
  stream->backend = context->backend.get();
  stream->inputs = std::move(actual.input_map);
  stream->outputs = std::move(actual.output_map);
  stream->format.struct_size = sizeof(ae_stream_format);
  stream->format.sample_rate = actual.sample_rate;
  stream->format.frames_per_block = actual.frames_per_block;
  stream->format.max_frames_per_block = actual.max_frames_per_block;
  stream->format.input_channels = actual.input_channels;
  stream->format.output_channels = actual.output_channels;
  stream->format.stream_id = context->next_stream_id++;
  stream->format.configuration_generation = context->next_configuration_generation++;

  ae_stream_format format = stream->format;
  format.struct_size = out_format->struct_size;
  *out_format = format;
  *out_stream = stream;
  context->active_streams = 1;
  return AE_OK;
}

}

extern "C" {

ae_result ae_get_api_version(uint32_t *out_major, uint32_t *out_minor) {
  if (out_major == nullptr || out_minor == nullptr) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  *out_major = AE_API_VERSION_MAJOR;
  *out_minor = AE_API_VERSION_MINOR;
  return AE_OK;
}

ae_result ae_context_create(const ae_context_options *options, ae_context **out_context) {
  return call([&]() -> ae_result {
    if (options == nullptr || out_context == nullptr) {
      return AE_ERROR_INVALID_ARGUMENT;
    }
    *out_context = nullptr;
    if (!has_required_size(options)) {
      return AE_ERROR_INCOMPATIBLE_ABI;
    }
    if (options->api_version_major != AE_API_VERSION_MAJOR ||
        options->api_version_minor != AE_API_VERSION_MINOR) {
      return AE_ERROR_INCOMPATIBLE_ABI;
    }
    return ae::detail::create_context_with_backend(std::make_unique<EmptyBackend>(), out_context);
  });
}

ae_result ae_context_destroy(ae_context *context) {
  return call([&]() -> ae_result {
    if (context == nullptr) {
      return AE_ERROR_INVALID_ARGUMENT;
    }
    if (context->active_streams != 0 || context->snapshots != 0) {
      return AE_ERROR_BUSY;
    }
    const ae_result result = context->backend->close();
    if (result != AE_OK) {
      return result;
    }
    delete context;
    return AE_OK;
  });
}

ae_result ae_context_refresh_devices(ae_context *context) {
  return call([&]() -> ae_result {
    if (context == nullptr) {
      return AE_ERROR_INVALID_ARGUMENT;
    }
    return context->backend->refresh();
  });
}

ae_result ae_context_get_diagnostic(const ae_context *context, ae_diagnostic *out) {
  if (context == nullptr || out == nullptr) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  if (!has_required_size(out)) {
    return AE_ERROR_INCOMPATIBLE_ABI;
  }
  auto diagnostic = context->backend->diagnostic();
  diagnostic.struct_size = out->struct_size;
  *out = diagnostic;
  return AE_OK;
}

ae_result ae_context_probe_device(ae_context *context, const char *backend_id,
                                  const char *device_id, ae_capability_snapshot **out) {
  return call([&]() -> ae_result {
    if (out == nullptr) {
      return AE_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;
    if (context == nullptr || !non_empty(backend_id) || !non_empty(device_id)) {
      return AE_ERROR_INVALID_ARGUMENT;
    }
    if (context->active_streams != 0) {
      return AE_ERROR_BUSY;
    }
    if (find_device(context, backend_id, device_id) == nullptr) {
      return AE_ERROR_NOT_FOUND;
    }
    auto snapshot = std::make_unique<ae_capability_snapshot>();
    const auto result = context->backend->probe(device_id, snapshot->capabilities);
    if (result != AE_OK) {
      return result;
    }
    snapshot->context = context;
    ++context->snapshots;
    *out = snapshot.release();
    return AE_OK;
  });
}

ae_result ae_capability_snapshot_get(const ae_capability_snapshot *snapshot,
                                     ae_device_capabilities *out) {
  if (snapshot == nullptr || out == nullptr) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  if (!has_required_size(out)) {
    return AE_ERROR_INCOMPATIBLE_ABI;
  }
  auto info = snapshot->capabilities.info;
  info.struct_size = out->struct_size;
  info.tested_rate_count = static_cast<uint32_t>(snapshot->capabilities.rates.size());
  info.tested_rates = snapshot->capabilities.rates.data();
  *out = info;
  return AE_OK;
}

ae_result ae_capability_snapshot_get_channel(const ae_capability_snapshot *snapshot,
                                             ae_direction direction, uint32_t index,
                                             ae_channel_info *out) {
  if (snapshot == nullptr || out == nullptr ||
      (direction != AE_DIRECTION_INPUT && direction != AE_DIRECTION_OUTPUT)) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  if (!has_required_size(out)) {
    return AE_ERROR_INCOMPATIBLE_ABI;
  }
  const auto &channels = direction == AE_DIRECTION_INPUT ? snapshot->capabilities.inputs
                                                         : snapshot->capabilities.outputs;
  if (index >= channels.size()) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  auto channel = channels[index];
  channel.struct_size = out->struct_size;
  *out = channel;
  return AE_OK;
}

ae_result ae_capability_snapshot_release(ae_capability_snapshot *snapshot) {
  if (snapshot == nullptr) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  --snapshot->context->snapshots;
  delete snapshot;
  return AE_OK;
}

ae_result ae_context_enumerate_devices(const ae_context *context,
                                       ae_device_snapshot **out_snapshot) {
  return call([&]() -> ae_result { return enumerate_devices(context, out_snapshot); });
}

uint32_t ae_device_snapshot_count(const ae_device_snapshot *snapshot) {
  return snapshot == nullptr ? 0 : static_cast<uint32_t>(snapshot->infos.size());
}

ae_result ae_device_snapshot_get(const ae_device_snapshot *snapshot, uint32_t index,
                                 ae_device_info *out_info) {
  if (snapshot == nullptr || out_info == nullptr) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  if (!has_required_size(out_info)) {
    return AE_ERROR_INCOMPATIBLE_ABI;
  }
  if (index >= snapshot->infos.size()) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  ae_device_info info = snapshot->infos[index];
  info.struct_size = out_info->struct_size;
  *out_info = info;
  return AE_OK;
}

ae_result ae_device_snapshot_release(ae_device_snapshot *snapshot) {
  if (snapshot == nullptr) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  if (snapshot->context != nullptr && snapshot->context->snapshots > 0) {
    --snapshot->context->snapshots;
  }
  delete snapshot;
  return AE_OK;
}

ae_result ae_stream_open(ae_context *context, const ae_stream_request *request,
                         ae_stream **out_stream, ae_stream_format *out_format) {
  return call([&]() -> ae_result { return open_stream(context, request, out_stream, out_format); });
}

ae_result ae_stream_get_format(const ae_stream *stream, ae_stream_format *out_format) {
  if (stream == nullptr || out_format == nullptr) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  if (!has_required_size(out_format)) {
    return AE_ERROR_INCOMPATIBLE_ABI;
  }
  ae_stream_format format = stream->format;
  format.struct_size = out_format->struct_size;
  *out_format = format;
  return AE_OK;
}

ae_result ae_stream_start(ae_stream *stream, ae_process_fn process, void *user) {
  return call([&]() -> ae_result {
    if (stream == nullptr || process == nullptr) {
      return AE_ERROR_INVALID_ARGUMENT;
    }
    if (stream->state.load(std::memory_order_acquire) != AE_STREAM_STATE_READY ||
        stream->backend->faults() != 0) {
      return AE_ERROR_INVALID_STATE;
    }

    stream->process = process;
    stream->user = user;
    stream->failure_latched.store(false, std::memory_order_relaxed);
    stream->last_result.store(AE_OK, std::memory_order_relaxed);
    stream->delivered_frames.store(0, std::memory_order_relaxed);
    stream->run_generation.fetch_add(1, std::memory_order_relaxed);
    stream->state.store(AE_STREAM_STATE_RUNNING, std::memory_order_release);

    ae_result result = call([&] { return stream->backend->start(&dispatch, stream); });
    if (result == AE_ERROR_QUIESCENCE_FAILURE) {
      stream->failure_latched.store(true, std::memory_order_release);
      stream->last_result.store(result, std::memory_order_relaxed);
      stream->state.store(AE_STREAM_STATE_RECOVERY_REQUIRED, std::memory_order_release);
      return result;
    }
    if (result != AE_OK) {
      stream->run_generation.fetch_sub(1, std::memory_order_relaxed);
      stream->process = nullptr;
      stream->user = nullptr;
      stream->failure_latched.store(false, std::memory_order_relaxed);
      stream->delivered_frames.store(0, std::memory_order_relaxed);
      stream->last_result.store(result, std::memory_order_relaxed);
      stream->state.store(AE_STREAM_STATE_READY, std::memory_order_release);
    }
    return result;
  });
}

ae_result ae_stream_stop(ae_stream *stream) {
  return call([&]() -> ae_result {
    if (stream == nullptr) {
      return AE_ERROR_INVALID_ARGUMENT;
    }
    if (stream->state.load(std::memory_order_acquire) == AE_STREAM_STATE_READY) {
      return AE_OK;
    }

    ae_result result = call([&] { return stream->backend->stop(); });
    if (result != AE_OK) {
      stream->last_result.store(result, std::memory_order_relaxed);
      return result;
    }
    stream->process = nullptr;
    stream->user = nullptr;
    stream->failure_latched.store(false, std::memory_order_relaxed);
    stream->state.store(stream->backend->faults() == 0 ? AE_STREAM_STATE_READY
                                                       : AE_STREAM_STATE_RECOVERY_REQUIRED,
                        std::memory_order_release);
    return AE_OK;
  });
}

ae_result ae_stream_get_status(const ae_stream *stream, ae_stream_status *out_status) {
  if (stream == nullptr || out_status == nullptr) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  if (!has_required_size(out_status)) {
    return AE_ERROR_INCOMPATIBLE_ABI;
  }

  ae_stream_status status{};
  status.struct_size = out_status->struct_size;
  status.state = stream->state.load(std::memory_order_acquire);
  status.last_result = stream->last_result.load(std::memory_order_relaxed);
  status.stream_id = stream->format.stream_id;
  status.configuration_generation = stream->format.configuration_generation;
  status.run_generation = stream->run_generation.load(std::memory_order_relaxed);
  status.delivered_frames = stream->delivered_frames.load(std::memory_order_relaxed);
  stream->backend->observations(status);
  status.fault_flags = stream->backend->faults();
  if (status.fault_flags != 0) {
    status.state = AE_STREAM_STATE_RECOVERY_REQUIRED;
    status.last_result = AE_ERROR_DEVICE_UNAVAILABLE;
  }
  *out_status = status;
  return AE_OK;
}

ae_result ae_stream_close(ae_stream *stream) {
  return call([&]() -> ae_result {
    if (stream == nullptr) {
      return AE_ERROR_INVALID_ARGUMENT;
    }
    if (ae_stream_stop(stream) != AE_OK) {
      return AE_ERROR_QUIESCENCE_FAILURE;
    }

    const ae_result result = stream->backend->close();
    if (result != AE_OK) {
      return result;
    }
    if (stream->context->active_streams > 0) {
      --stream->context->active_streams;
    }
    delete stream;
    return AE_OK;
  });
}

ae_result ae_stream_get_channel(const ae_stream *stream, ae_direction direction, uint32_t index,
                                uint32_t *out) {
  if (stream == nullptr || out == nullptr ||
      (direction != AE_DIRECTION_INPUT && direction != AE_DIRECTION_OUTPUT)) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  const auto &channels = direction == AE_DIRECTION_INPUT ? stream->inputs : stream->outputs;
  if (index >= channels.size()) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  *out = channels[index];
  return AE_OK;
}
}

namespace ae::detail {

ae_result create_context_with_backend(std::unique_ptr<Backend> backend, ae_context **out_context) {
  if (backend == nullptr || out_context == nullptr) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  *out_context = nullptr;

  auto *context = new (std::nothrow) ae_context();
  if (context == nullptr) {
    return AE_ERROR_ALLOCATION_FAILURE;
  }
  context->backend = std::move(backend);
  *out_context = context;
  return AE_OK;
}

}
