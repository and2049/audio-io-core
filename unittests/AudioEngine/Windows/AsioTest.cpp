#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

#include "Check.h"
#include "backends/windows/asio_backend.h"
#include "backends/windows/asio_timing.h"
#include "testing_api.h"

thread_local bool forbid_allocation = false;

void *operator new(size_t size) {
  AE_CHECK(!forbid_allocation);
  void *pointer = std::malloc(size == 0 ? 1 : size);
  if (pointer == nullptr) {
    throw std::bad_alloc();
  }
  return pointer;
}
void *operator new[](size_t size) { return ::operator new(size); }
void operator delete(void *pointer) noexcept { std::free(pointer); }
void operator delete[](void *pointer) noexcept { std::free(pointer); }
void operator delete(void *pointer, size_t) noexcept { std::free(pointer); }
void operator delete[](void *pointer, size_t) noexcept { std::free(pointer); }
void *operator new(size_t size, std::align_val_t alignment) {
  AE_CHECK(!forbid_allocation);
  void *pointer = _aligned_malloc(size == 0 ? 1 : size, static_cast<size_t>(alignment));
  if (pointer == nullptr) {
    throw std::bad_alloc();
  }
  return pointer;
}
void *operator new[](size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}
void operator delete(void *pointer, std::align_val_t) noexcept { _aligned_free(pointer); }
void operator delete[](void *pointer, std::align_val_t) noexcept { _aligned_free(pointer); }
void operator delete(void *pointer, size_t, std::align_val_t) noexcept { _aligned_free(pointer); }
void operator delete[](void *pointer, size_t, std::align_val_t) noexcept { _aligned_free(pointer); }

namespace {
constexpr const char *identity = "{AC4D0455-50D7-4498-B3CD-9A41D130B759}";

struct State {
  DWORD owner = 0;
  uint32_t activations = 0, releases = 0, disposals = 0;
  bool fail_init = false, fail_create = false, fail_start = false, fail_stop = false;
  bool fail_dispose = false, threaded = false, null_buffer = false;
  bool fail_create_empty = false, invalid_dispose = false;
  bool change_rate_during_create = false, change_map_during_create = false;
  bool notify_set_rate = false, notify_create_rate = false, notify_query_rate = false;
  bool notify_latency_rate = false;
  double notified_rate = 96000;
  ASIOSampleType created_type = ASIOSTInt32LSB;
  bool change_type_during_create = false, fail_final_channel_info = false;
  bool early_stop = false;
  bool ramp_input = false;
  long minimum = 64, maximum = 256, preferred = 128, granularity = -1;
  std::atomic<bool> stop_entered{false};
  ASIOBool direct = ASIOTrue;
  ASIOSampleType type = ASIOSTInt32LSB;
  ASIOCallbacks callbacks{};
  std::vector<ASIOBufferInfo> buffers;
  std::vector<std::vector<int32_t>> storage;
  std::atomic<uint32_t> output_ready{0};
} state;

void control_thread() { AE_CHECK(GetCurrentThreadId() == state.owner); }

int32_t ramp_sample(long channel, long half, uint32_t frame) {
  const int32_t magnitude = 0x10000000 + static_cast<int32_t>(half) * 0x01000000 +
                            static_cast<int32_t>(frame) * 0x00010000;
  return channel == 0 ? magnitude : -magnitude;
}

class Driver final : public IASIO {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **) override { return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override {
    control_thread();
    if (thread_.joinable()) {
      thread_.join();
    }
    ++state.releases;
    delete this;
    return 0;
  }
  ASIOBool init(void *window) override {
    control_thread();
    AE_CHECK(IsWindow(static_cast<HWND>(window)));
    return state.fail_init ? ASIOFalse : ASIOTrue;
  }
  void getDriverName(char *name) override { std::memcpy(name, "Fake ASIO", sizeof("Fake ASIO")); }
  long getDriverVersion() override { return 1; }
  void getErrorMessage(char *message) override {
    std::memcpy(message, "injected error", sizeof("injected error"));
  }
  ASIOError start() override {
    control_thread();
    if (state.threaded) {
      thread_ = std::thread([] { state.callbacks.bufferSwitch(0, state.direct); });
    } else {
      forbid_allocation = true;
      state.callbacks.bufferSwitch(0, state.direct);
      forbid_allocation = false;
    }
    return state.fail_start ? ASE_HWMalfunction : ASE_OK;
  }
  ASIOError stop() override {
    control_thread();
    state.stop_entered.store(true);
    state.stop_entered.notify_all();
    if (state.fail_stop) {
      return ASE_HWMalfunction;
    }
    if (thread_.joinable() && !state.early_stop) {
      thread_.join();
    }
    return ASE_OK;
  }
  ASIOError getChannels(long *input, long *output) override {
    control_thread();
    *input = 2;
    *output = 2;
    return ASE_OK;
  }
  ASIOError getLatencies(long *input, long *output) override {
    control_thread();
    *input = 32;
    *output = 64;
    if (state.notify_latency_rate) {
      rate_ = state.notified_rate;
      state.callbacks.sampleRateDidChange(rate_);
    }
    return ASE_OK;
  }
  ASIOError getBufferSize(long *min, long *max, long *preferred, long *granularity) override {
    control_thread();
    *min = state.minimum;
    *max = state.maximum;
    *preferred = state.preferred;
    *granularity = state.granularity;
    return ASE_OK;
  }
  ASIOError canSampleRate(ASIOSampleRate rate) override {
    control_thread();
    return rate == 44100 || rate == 48000 ? ASE_OK : ASE_NoClock;
  }
  ASIOError getSampleRate(ASIOSampleRate *rate) override {
    control_thread();
    *rate = rate_;
    if (created_ && state.notify_query_rate) {
      rate_ = state.notified_rate;
      state.callbacks.sampleRateDidChange(rate_);
    }
    return ASE_OK;
  }
  ASIOError setSampleRate(ASIOSampleRate rate) override {
    control_thread();
    rate_ = rate;
    if (state.notify_set_rate) {
      state.callbacks.sampleRateDidChange(rate_);
    }
    return ASE_OK;
  }
  ASIOError getClockSources(ASIOClockSource *, long *) override { return ASE_NotPresent; }
  ASIOError setClockSource(long) override { return ASE_NotPresent; }
  ASIOError getSamplePosition(ASIOSamples *, ASIOTimeStamp *) override { return ASE_NotPresent; }
  ASIOError getChannelInfo(ASIOChannelInfo *info) override {
    control_thread();
    if (created_ && state.fail_final_channel_info) {
      return ASE_HWMalfunction;
    }
    info->type = created_ && state.change_type_during_create ? state.created_type : state.type;
    std::memcpy(info->name, "Fake channel", sizeof("Fake channel"));
    return ASE_OK;
  }
  ASIOError createBuffers(ASIOBufferInfo *buffers, long channels, long frames,
                          ASIOCallbacks *callbacks) override {
    control_thread();
    state.callbacks = *callbacks;
    if (state.fail_create_empty) {
      return ASE_NoMemory;
    }
    created_ = true;
    AE_CHECK(callbacks->asioMessage(kAsioEngineVersion, 0, nullptr, nullptr) == 2);
    callbacks->bufferSwitch(0, ASIOTrue);
    state.storage.resize(static_cast<size_t>(channels) * 2);
    for (long i = 0; i < channels; ++i) {
      for (long half = 0; half < 2; ++half) {
        auto &storage = state.storage[static_cast<size_t>(i * 2 + half)];
        storage.assign(static_cast<size_t>(frames), buffers[i].isInput ? 0x20000000 : 0x12345678);
        buffers[i].buffers[half] = state.null_buffer ? nullptr : storage.data();
        if (state.ramp_input) {
          storage.assign(static_cast<size_t>(frames) + 2, 0x12345678);
          if (buffers[i].isInput) {
            for (uint32_t frame = 0; frame < static_cast<uint32_t>(frames); ++frame) {
              storage[frame + 1] = ramp_sample(buffers[i].channelNum, half, frame);
            }
          }
          buffers[i].buffers[half] = storage.data() + 1;
        }
      }
    }
    state.buffers.assign(buffers, buffers + channels);
    if (state.change_rate_during_create) {
      rate_ = 96000;
    }
    if (state.change_map_during_create) {
      buffers[0].channelNum = 0;
    }
    if (state.notify_create_rate) {
      callbacks->sampleRateDidChange(state.notified_rate);
    }
    return state.fail_create ? ASE_NoMemory : ASE_OK;
  }
  ASIOError disposeBuffers() override {
    control_thread();
    ++state.disposals;
    if (state.fail_dispose) {
      return ASE_HWMalfunction;
    }
    if (!created_ || state.invalid_dispose) {
      return ASE_InvalidMode;
    }
    created_ = false;
    return ASE_OK;
  }
  ASIOError controlPanel() override { return ASE_NotPresent; }
  ASIOError future(long, void *) override { return ASE_NotPresent; }
  ASIOError outputReady() override {
    ++state.output_ready;
    return ASE_OK;
  }

private:
  bool created_ = false;
  double rate_ = 48000;
  std::thread thread_;
};

HRESULT factory(const CLSID &, IASIO **out) {
  if (state.owner == 0) {
    state.owner = GetCurrentThreadId();
  }
  control_thread();
  ++state.activations;
  *out = new Driver;
  return S_OK;
}

ae_context *context() {
  ae_context *result = nullptr;
  auto backend = ae::make_asio_backend(factory, {{"asio", identity, "Fake", 0, 0, false}});
  AE_CHECK(ae::detail::create_context_with_backend(std::move(backend), &result) == AE_OK);
  return result;
}

ae_stream_request request() {
  static const uint32_t input[] = {1};
  static const uint32_t output[] = {1, 0};
  ae_stream_request result{};
  result.struct_size = sizeof(result);
  result.backend_id = "asio";
  result.input_device_id = identity;
  result.output_device_id = identity;
  result.input_channels = {AE_CHANNEL_MODE_EXPLICIT, 1, input};
  result.output_channels = {AE_CHANNEL_MODE_EXPLICIT, 2, output};
  result.sample_rate = 48000;
  result.frames_per_block = 128;
  return result;
}

struct Processor {
  std::atomic<uint32_t> calls{0};
  std::atomic<bool> entered{false}, release{true};
};

ae_process_result process(void *user, const ae_audio_block *block) {
  auto &processor = *static_cast<Processor *>(user);
  ++processor.calls;
  processor.entered.store(true);
  processor.entered.notify_all();
  while (!processor.release.load()) {
    processor.release.wait(false);
  }
  AE_CHECK(block->input_channels == 1 && block->output_channels == 2 && block->frames == 128);
  for (uint32_t i = 0; i < block->frames; ++i) {
    AE_CHECK(block->inputs[0][i] == 0.25f);
    AE_CHECK(block->outputs[0][i] == 0.0f);
    block->outputs[0][i] = block->inputs[0][i] * 2;
    block->outputs[1][i] = -0.5f;
  }
  return AE_PROCESS_OK;
}

void lifecycle() {
  auto *ctx = context();
  ae_capability_snapshot *caps = nullptr;
  AE_CHECK(ae_context_probe_device(ctx, "asio", identity, &caps) == AE_OK);
  AE_CHECK(state.activations == 1 && state.releases == 1);
  ae_channel_info channel{};
  channel.struct_size = sizeof(channel);
  AE_CHECK(ae_capability_snapshot_get_channel(caps, AE_DIRECTION_INPUT, 1, &channel) == AE_OK);
  AE_CHECK(channel.supported == 1 && channel.native_format == ASIOSTInt32LSB);
  AE_CHECK(ae_capability_snapshot_release(caps) == AE_OK);
  auto spec = request();
  ae_stream_format format{};
  format.struct_size = sizeof(format);
  ae_stream *stream = nullptr;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  AE_CHECK(state.buffers[0].channelNum == 1 && state.buffers[1].channelNum == 1 &&
           state.buffers[2].channelNum == 0);
  AE_CHECK(state.storage[2][0] == 0 && state.storage[3][0] == 0);
  auto *other = context();
  AE_CHECK(ae_context_probe_device(other, "asio", identity, &caps) == AE_ERROR_BUSY);
  AE_CHECK(ae_context_destroy(other) == AE_OK);
  Processor processor;
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_OK);
  AE_CHECK(processor.calls == 1);
  AE_CHECK(state.storage[2][0] == 0x40000000 && state.storage[4][0] == -0x40000000);
  state.callbacks.asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
  ae_stream_status status{};
  status.struct_size = sizeof(status);
  AE_CHECK(ae_stream_get_status(stream, &status) == AE_OK);
  AE_CHECK(status.fault_flags == AE_FAULT_RESET && status.latency_valid == 1);
  AE_CHECK(status.last_result == AE_ERROR_CONFIGURATION_CHANGED);
  AE_CHECK(status.input_latency_frames == 32 && status.callback_count == 1);
  state.callbacks.bufferSwitch(1, ASIOTrue);
  AE_CHECK(processor.calls == 1 && state.storage[3][0] == 0);
  AE_CHECK(ae_stream_stop(stream) == AE_OK);
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_ERROR_INVALID_STATE);
  state.fail_dispose = true;
  AE_CHECK(ae_stream_close(stream) == AE_ERROR_DRIVER_FAILURE);
  AE_CHECK(ae_context_destroy(ctx) == AE_ERROR_BUSY);
  state.fail_dispose = false;
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
  state.callbacks.bufferSwitch(0, ASIOTrue);
}

void failures() {
  state.owner = 0;
  auto *ctx = context();
  auto spec = request();
  ae_stream_format format{};
  format.struct_size = sizeof(format);
  ae_stream *stream = nullptr;
  state.fail_init = true;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_DEVICE_UNAVAILABLE);
  state.fail_init = false;
  state.type = ASIOSTDSDInt8LSB1;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_UNSUPPORTED_CONFIGURATION);
  state.type = ASIOSTInt32LSB;
  spec.frames_per_block = 100;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_UNSUPPORTED_CONFIGURATION);
  spec.frames_per_block = 128;
  state.granularity = 0;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_DRIVER_FAILURE);
  state.granularity = 16;
  spec.frames_per_block = 112;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  AE_CHECK(format.frames_per_block == 112);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  spec.frames_per_block = 113;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_UNSUPPORTED_CONFIGURATION);
  spec.frames_per_block = 128;
  state.granularity = -1;
  state.change_rate_during_create = true;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_UNSUPPORTED_CONFIGURATION);
  state.change_rate_during_create = false;
  state.change_map_during_create = true;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_DRIVER_FAILURE);
  state.change_map_during_create = false;
  state.fail_create = true;
  state.fail_dispose = true;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_DRIVER_FAILURE);
  AE_CHECK(stream == nullptr);
  AE_CHECK(ae_context_destroy(ctx) == AE_ERROR_DRIVER_FAILURE);
  state.fail_create = false;
  state.fail_dispose = false;
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
  state.owner = 0;
  ctx = context();
  state.null_buffer = true;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_DRIVER_FAILURE);
  state.null_buffer = false;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  Processor processor;
  state.fail_start = true;
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_ERROR_DRIVER_FAILURE);
  ae_stream_status rollback{};
  rollback.struct_size = sizeof(rollback);
  AE_CHECK(ae_stream_get_status(stream, &rollback) == AE_OK);
  AE_CHECK(rollback.state == AE_STREAM_STATE_READY && rollback.run_generation == 1);
  state.fail_stop = true;
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_ERROR_QUIESCENCE_FAILURE);
  AE_CHECK(ae_stream_close(stream) == AE_ERROR_QUIESCENCE_FAILURE);
  state.fail_stop = false;
  state.fail_start = false;
  AE_CHECK(ae_stream_stop(stream) == AE_OK);
  state.direct = ASIOFalse;
  const uint32_t calls = processor.calls.load();
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_OK);
  AE_CHECK(processor.calls == calls);
  ae_stream_status status{};
  status.struct_size = sizeof(status);
  AE_CHECK(ae_stream_get_status(stream, &status) == AE_OK &&
           status.fault_flags == AE_FAULT_CALLBACK);
  state.direct = ASIOTrue;
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
}

void in_flight_stop() {
  state.owner = 0;
  state.threaded = true;
  state.early_stop = true;
  auto *ctx = context();
  auto spec = request();
  ae_stream_format format{};
  format.struct_size = sizeof(format);
  ae_stream *stream = nullptr;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  Processor processor;
  processor.release.store(false);
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_OK);
  while (!processor.entered.load()) {
    processor.entered.wait(false);
  }
  state.callbacks.sampleRateDidChange(0);
  ae_stream_status status{};
  status.struct_size = sizeof(status);
  AE_CHECK(ae_stream_get_status(stream, &status) == AE_OK);
  AE_CHECK((status.fault_flags & AE_FAULT_CLOCK_LOST) != 0);
  state.stop_entered.store(false);
  std::atomic<bool> stopped{false};
  std::thread stopper([&] {
    AE_CHECK(ae_stream_stop(stream) == AE_OK);
    stopped.store(true);
  });
  while (!state.stop_entered.load()) {
    state.stop_entered.wait(false);
  }
  AE_CHECK(!stopped.load());
  processor.release.store(true);
  processor.release.notify_all();
  stopper.join();
  AE_CHECK(stopped.load());
  AE_CHECK(state.storage[2][0] == 0);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
}

void preparation_changes() {
  state.owner = 0;
  auto *ctx = context();
  auto spec = request();
  ae_stream_format format{};
  format.struct_size = sizeof(format);
  ae_stream *stream = nullptr;
  for (bool *notification :
       {&state.notify_create_rate, &state.notify_query_rate, &state.notify_latency_rate}) {
    for (double rate : {0.0, 48000.0, 96000.0}) {
      state.notified_rate = rate;
      *notification = true;
      AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_CONFIGURATION_CHANGED);
      AE_CHECK(stream == nullptr);
      *notification = false;
    }
  }
  state.notify_set_rate = true;
  spec.sample_rate = 44100;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  AE_CHECK(format.sample_rate == 44100);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  state.notify_set_rate = false;
  spec.sample_rate = 48000;
  state.change_type_during_create = true;
  for (ASIOSampleType type :
       {ASIOSTFloat32LSB, ASIOSTInt16LSB, ASIOSTInt32MSB, ASIOSTDSDInt8LSB1}) {
    state.created_type = type;
    AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_CONFIGURATION_CHANGED);
    AE_CHECK(stream == nullptr);
    AE_CHECK(state.storage[2][0] == 0x12345678);
  }
  state.change_type_during_create = false;
  state.fail_final_channel_info = true;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_DRIVER_FAILURE);
  state.fail_final_channel_info = false;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  state.callbacks.sampleRateDidChange(96000);
  Processor processor;
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_ERROR_INVALID_STATE);
  AE_CHECK(processor.calls == 0);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
}

void watchdog_timing() {
  const uint64_t frequency = 10000000;
  const uint64_t timeout = ae::detail::asio_stall_timeout(frequency, 128, 48000);
  AE_CHECK(timeout == frequency * 2);
  std::atomic<uint64_t> last{frequency};
  AE_CHECK(!ae::detail::asio_callback_stalled(last, timeout, [&] { return frequency - 1; }));
  AE_CHECK(!ae::detail::asio_callback_stalled(last, timeout, [&] { return frequency + timeout; }));
  AE_CHECK(
      ae::detail::asio_callback_stalled(last, timeout, [&] { return frequency + timeout + 1; }));
  AE_CHECK(!ae::detail::asio_callback_stalled(last, timeout, [&] {
    last.store(frequency + 2);
    return frequency + 1;
  }));
  AE_CHECK(!ae::detail::asio_callback_stalled(last, timeout, [&] {
    const auto current = frequency + timeout + 10;
    last.store(current);
    return current;
  }));
  const uint64_t period = frequency * 65536 / 8000;
  const uint64_t long_timeout = ae::detail::asio_stall_timeout(frequency, 65536, 8000);
  AE_CHECK(long_timeout == period * 3);
  last.store(0);
  AE_CHECK(!ae::detail::asio_callback_stalled(last, long_timeout, [&] { return period; }));
  AE_CHECK(ae::detail::asio_callback_stalled(last, long_timeout, [&] { return long_timeout + 1; }));
}

void allocation_cleanup() {
  state.owner = 0;
  auto *ctx = context();
  auto spec = request();
  ae_stream_format format{};
  format.struct_size = sizeof(format);
  ae_stream *stream = nullptr;
  const uint32_t releases = state.releases;
  state.fail_create_empty = true;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_DRIVER_FAILURE);
  AE_CHECK(stream == nullptr && state.releases == releases + 1);
  ae_diagnostic diagnostic{};
  diagnostic.struct_size = sizeof(diagnostic);
  AE_CHECK(ae_context_get_diagnostic(ctx, &diagnostic) == AE_OK);
  AE_CHECK(diagnostic.native_code == ASE_NoMemory);
  state.fail_dispose = true;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_DRIVER_FAILURE);
  AE_CHECK(ae_context_destroy(ctx) == AE_ERROR_DRIVER_FAILURE);
  state.fail_dispose = false;
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
  state.fail_create_empty = false;
  state.owner = 0;
  ctx = context();
  state.fail_create = true;
  state.invalid_dispose = true;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_DRIVER_FAILURE);
  AE_CHECK(ae_context_destroy(ctx) == AE_ERROR_DRIVER_FAILURE);
  state.fail_create = false;
  state.invalid_dispose = false;
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
  state.owner = 0;
  ctx = context();
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  state.invalid_dispose = true;
  AE_CHECK(ae_stream_close(stream) == AE_ERROR_DRIVER_FAILURE);
  state.invalid_dispose = false;
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
}

struct RoutingProcessor {
  uint32_t calls = 0;
  uint32_t half = 0;
  uint64_t position = 0;
  ae_process_result result = AE_PROCESS_OK;
  bool partial = false;
};

ae_process_result route_ramps(void *user, const ae_audio_block *block) {
  auto &processor = *static_cast<RoutingProcessor *>(user);
  AE_CHECK(block->input_channels == 2 && block->output_channels == 2 && block->frames == 128);
  AE_CHECK(block->frame_position == processor.position);
  processor.position += block->frames;
  ++processor.calls;
  for (uint32_t frame = 0; frame < block->frames; ++frame) {
    const float magnitude =
        0.125f + static_cast<float>(processor.half) / 128.0f + static_cast<float>(frame) / 32768.0f;
    AE_CHECK(block->inputs[0][frame] == -magnitude);
    AE_CHECK(block->inputs[1][frame] == magnitude);
    AE_CHECK(block->outputs[0][frame] == 0.0f && block->outputs[1][frame] == 0.0f);
    if (!processor.partial || frame + 1 < block->frames) {
      block->outputs[0][frame] = block->inputs[1][frame];
      block->outputs[1][frame] = block->inputs[0][frame];
    }
  }
  return processor.result;
}

void channel_routing() {
  state.owner = 0;
  state.ramp_input = true;
  auto *ctx = context();
  auto spec = request();
  const uint32_t inputs[] = {1, 0};
  spec.input_channels = {AE_CHANNEL_MODE_EXPLICIT, 2, inputs};
  ae_stream_format format{};
  format.struct_size = sizeof(format);
  ae_stream *stream = nullptr;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  for (ae_direction direction : {AE_DIRECTION_INPUT, AE_DIRECTION_OUTPUT}) {
    for (uint32_t channel = 0; channel < 2; ++channel) {
      uint32_t physical = 0;
      AE_CHECK(ae_stream_get_channel(stream, direction, channel, &physical) == AE_OK);
      AE_CHECK(physical == 1 - channel);
    }
  }
  RoutingProcessor processor;
  for (uint32_t cycle = 0; cycle < 2; ++cycle) {
    processor.position = 0;
    processor.half = 0;
    processor.partial = false;
    processor.result = AE_PROCESS_OK;
    AE_CHECK(ae_stream_start(stream, route_ramps, &processor) == AE_OK);
    for (uint32_t block = 0; block < 6; ++block) {
      processor.half = block % 2;
      processor.partial = block == 2;
      processor.result = block == 3   ? AE_PROCESS_SILENCE
                         : block == 4 ? AE_PROCESS_FAILED
                                      : AE_PROCESS_OK;
      const uint32_t previous_calls = processor.calls;
      if (block != 0) {
        state.callbacks.bufferSwitch(processor.half, ASIOTrue);
        AE_CHECK(processor.calls == previous_calls + (block < 5 ? 1 : 0));
      }
      for (size_t buffer = 0; buffer < state.buffers.size(); ++buffer) {
        const auto &native = state.buffers[buffer];
        const auto &storage = state.storage[buffer * 2 + processor.half];
        AE_CHECK(storage.front() == 0x12345678 && storage.back() == 0x12345678);
        for (uint32_t frame = 0; frame < 128; ++frame) {
          const int32_t expected = native.isInput
                                       ? ramp_sample(native.channelNum, processor.half, frame)
                                   : block >= 3 || (processor.partial && frame == 127)
                                       ? 0
                                       : ramp_sample(1 - native.channelNum, processor.half, frame);
          AE_CHECK(storage[frame + 1] == expected);
        }
      }
    }
    AE_CHECK(ae_stream_stop(stream) == AE_OK);
  }
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
  state.ramp_input = false;
}
}

namespace {
struct RunHistory {
  uint32_t generation = 0;
  uint32_t resets = 0;
  uint32_t calls = 0;
  uint64_t frames = 0;
};

ae_process_result track_run(void *user, const ae_audio_block *block) {
  auto &history = *static_cast<RunHistory *>(user);
  if (history.generation != block->run_generation) {
    history.generation = block->run_generation;
    history.frames = 0;
    ++history.resets;
  }
  AE_CHECK(block->frame_position == history.frames);
  history.frames += block->frames;
  ++history.calls;
  return AE_PROCESS_SILENCE;
}

void start_attempt_identity() {
  state.owner = 0;
  state.threaded = false;
  state.early_stop = false;
  auto *ctx = context();
  auto spec = request();
  ae_stream_format format{};
  format.struct_size = sizeof(format);
  ae_stream *stream = nullptr;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  RunHistory history;
  state.fail_start = true;
  AE_CHECK(ae_stream_start(stream, track_run, &history) == AE_ERROR_DRIVER_FAILURE);
  AE_CHECK(history.calls == 1 && history.resets == 1 && history.generation == 1);
  state.fail_start = false;
  AE_CHECK(ae_stream_start(stream, track_run, &history) == AE_OK);
  AE_CHECK(history.calls == 2 && history.resets == 2 && history.generation == 2);
  AE_CHECK(history.frames == format.frames_per_block);
  AE_CHECK(ae_stream_stop(stream) == AE_OK);
  AE_CHECK(ae_stream_start(stream, track_run, &history) == AE_OK);
  AE_CHECK(history.calls == 3 && history.resets == 3 && history.generation == 3);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
}

void unsupported_buffer_resize() {
  state.owner = 0;
  auto *ctx = context();
  auto spec = request();
  ae_stream_format format{};
  format.struct_size = sizeof(format);
  ae_stream *stream = nullptr;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  Processor processor;
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_OK);
  AE_CHECK(state.callbacks.asioMessage(kAsioSelectorSupported, kAsioBufferSizeChange, nullptr,
                                       nullptr) == 0);
  AE_CHECK(state.callbacks.asioMessage(kAsioBufferSizeChange, 64, nullptr, nullptr) == 0);
  ae_stream_status status{};
  status.struct_size = sizeof(status);
  AE_CHECK(ae_stream_get_status(stream, &status) == AE_OK);
  AE_CHECK(status.state == AE_STREAM_STATE_RUNNING && status.fault_flags == 0);
  AE_CHECK(ae_stream_get_format(stream, &format) == AE_OK && format.frames_per_block == 128);
  state.callbacks.bufferSwitch(1, ASIOTrue);
  AE_CHECK(processor.calls == 2);
  AE_CHECK(state.callbacks.asioMessage(kAsioSelectorSupported, kAsioResetRequest, nullptr,
                                       nullptr) == 1);
  AE_CHECK(state.callbacks.asioMessage(kAsioResetRequest, 0, nullptr, nullptr) == 1);
  AE_CHECK(ae_stream_get_status(stream, &status) == AE_OK && status.fault_flags == AE_FAULT_RESET);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
}

}

int main() {
  static_assert(sizeof(long) == 4 && sizeof(ASIOSamples) == 8 && sizeof(ASIOBufferInfo) == 24);
  lifecycle();
  failures();
  preparation_changes();
  watchdog_timing();
  allocation_cleanup();
  channel_routing();
  in_flight_stop();
  start_attempt_identity();
  unsupported_buffer_resize();
}
