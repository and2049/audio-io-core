#include "audio_engine/engine.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {
ae_context *context = nullptr;

void require(ae_result result, const char *operation) {
  if (result == AE_OK) {
    return;
  }
  ae_diagnostic diagnostic{};
  diagnostic.struct_size = sizeof(diagnostic);
  ae_context_get_diagnostic(context, &diagnostic);
  std::fprintf(stderr, "%s: engine=%d native=%lld %s\n", operation, result,
               static_cast<long long>(diagnostic.native_code), diagnostic.message);
  std::exit(1);
}

uint32_t number(const char *text, uint32_t maximum) {
  uint32_t result = 0;
  const auto parsed = std::from_chars(text, text + std::strlen(text), result);
  if (parsed.ec != std::errc{} || *parsed.ptr != '\0' || result > maximum) {
    std::fprintf(stderr, "Invalid numeric argument: %s\n", text);
    std::exit(2);
  }
  return result;
}

struct Processor {
  float gain = 0;
  std::atomic<float> peak{0};
};
static_assert(std::atomic<float>::is_always_lock_free);

ae_process_result process(void *user, const ae_audio_block *block) {
  auto &processor = *static_cast<Processor *>(user);
  float peak = processor.peak.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < block->frames; ++i) {
    const float input = block->input_channels == 0 ? 0.0f : block->inputs[0][i];
    peak = std::max(peak, std::abs(input));
    for (uint32_t channel = 0; channel < block->output_channels; ++channel) {
      block->outputs[channel][i] = input * processor.gain;
    }
  }
  processor.peak.store(peak, std::memory_order_relaxed);
  return AE_PROCESS_OK;
}

constexpr float kToneLevel = 0.01f;
constexpr float kToneThreshold = 0.0025f;
constexpr uint64_t kUndetected = UINT64_MAX;

struct Tone {
  uint64_t start = 0;
  double step = 0;
  std::atomic<uint64_t> detected{kUndetected};
  std::atomic<float> peak{0};
};
static_assert(std::atomic<uint64_t>::is_always_lock_free);

ae_process_result tone(void *user, const ae_audio_block *block) {
  auto &state = *static_cast<Tone *>(user);
  float peak = state.peak.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < block->frames; ++i) {
    const uint64_t position = block->frame_position + i;
    const float input = block->input_channels == 0 ? 0.0f : std::abs(block->inputs[0][i]);
    if (position >= state.start) {
      peak = std::max(peak, input);
      if (input > kToneThreshold && state.detected.load(std::memory_order_relaxed) == kUndetected) {
        state.detected.store(position, std::memory_order_relaxed);
      }
      const float value =
          kToneLevel *
          static_cast<float>(std::sin(static_cast<double>(position - state.start) * state.step));
      for (uint32_t channel = 0; channel < block->output_channels; ++channel) {
        block->outputs[channel][i] = value;
      }
    }
  }
  state.peak.store(peak, std::memory_order_relaxed);
  return AE_PROCESS_OK;
}

void print_status(ae_stream *stream, const Processor &processor) {
  ae_stream_status status{};
  status.struct_size = sizeof(status);
  require(ae_stream_get_status(stream, &status), "status");
  std::printf("state=%d faults=%u frames=%llu callbacks=%llu max_us=%.3f overruns=%llu "
              "overloads=%llu latency_valid=%u latency=%u/%u peak=%.6f\n",
              status.state, status.fault_flags,
              static_cast<unsigned long long>(status.delivered_frames),
              static_cast<unsigned long long>(status.callback_count),
              static_cast<double>(status.max_callback_nanoseconds) / 1000.0,
              static_cast<unsigned long long>(status.deadline_overruns),
              static_cast<unsigned long long>(status.driver_overloads), status.latency_valid,
              status.input_latency_frames, status.output_latency_frames, processor.peak.load());
  std::fflush(stdout);
}
}

int main(int argc, char **argv) {
  const bool probe = argc == 3 && std::strcmp(argv[1], "--probe") == 0;
  const bool pair = argc == 9 && std::strcmp(argv[1], "--pair") == 0;
  const bool loop = argc == 7 && std::strcmp(argv[1], "--tone") == 0;
  const bool run = pair || loop || (argc == 9 && std::strcmp(argv[1], "--run") == 0);
  if (argc != 1 && !probe && !run) {
    std::fprintf(stderr,
                 "Usage: AudioEngineDeviceCheck [--probe device-index | --run device-index seconds "
                 "rate frames input-index gain-percent cycles | --pair input-device-index "
                 "output-device-index seconds rate frames input-index gain-percent | --tone "
                 "device-index seconds rate frames input-index]\nDefault lists drivers. Gain 0 "
                 "produces silent output; gain 1..100 enables passthrough. --tone plays a 1 kHz "
                 "tone at -40 dBFS and reports when it returns on the input.\n");
    return 2;
  }
  const int shift = pair ? 1 : 0;
  const uint32_t selected = argc > 1 ? number(argv[2], 65535) : 0;
  const uint32_t selected_output = pair ? number(argv[3], 65535) : selected;
  const uint32_t seconds = run ? number(argv[3 + shift], 3600) : 0;
  const uint32_t rate = run ? number(argv[4 + shift], AE_MAX_SAMPLE_RATE) : 0;
  const uint32_t frames = run ? number(argv[5 + shift], AE_MAX_FRAMES_PER_BLOCK) : 0;
  const uint32_t input = run ? number(argv[6 + shift], AE_MAX_CHANNELS - 1) : 0;
  const uint32_t gain = run && !loop ? number(argv[7 + shift], 100) : 0;
  const uint32_t cycles = pair || loop ? 1 : run ? number(argv[8], 1000) : 0;
  if (run && (seconds == 0 || cycles == 0)) {
    return 2;
  }
  ae_context_options options{sizeof(options), AE_API_VERSION_MAJOR, AE_API_VERSION_MINOR, 0};
  require(ae_context_create(&options, &context), "create context");
  ae_device_snapshot *devices = nullptr;
  require(ae_context_enumerate_devices(context, &devices), "enumerate");
  std::string backend, id, output_id;
  for (uint32_t i = 0; i < ae_device_snapshot_count(devices); ++i) {
    ae_device_info info{};
    info.struct_size = sizeof(info);
    require(ae_device_snapshot_get(devices, i, &info), "device");
    std::printf("%u: %s | %s | %s%s\n", i, info.display_name, info.backend_id, info.device_id,
                (info.flags & AE_DEVICE_PAIRABLE) != 0 ? " | pairable" : "");
    if (i == selected) {
      backend = info.backend_id;
      id = info.device_id;
    }
    if (i == selected_output) {
      output_id = info.device_id;
    }
  }
  require(ae_device_snapshot_release(devices), "release devices");
  if (argc == 1) {
    require(ae_context_destroy(context), "destroy context");
    return 0;
  }
  if (id.empty() || output_id.empty()) {
    require(AE_ERROR_NOT_FOUND, "selected device");
  }
  ae_capability_snapshot *snapshot = nullptr;
  require(ae_context_probe_device(context, backend.c_str(), id.c_str(), &snapshot), "probe");
  ae_device_capabilities caps{};
  caps.struct_size = sizeof(caps);
  require(ae_capability_snapshot_get(snapshot, &caps), "capabilities");
  std::printf("rate=%u buffers=%u..%u preferred=%u granularity=%d channels=%u/%u\ntested_rates:",
              caps.current_sample_rate, caps.min_frames, caps.max_frames, caps.preferred_frames,
              caps.frame_granularity, caps.input_channels, caps.output_channels);
  for (uint32_t i = 0; i < caps.tested_rate_count; ++i) {
    std::printf(" %u", caps.tested_rates[i]);
  }
  std::printf("\n");
  for (ae_direction direction : {AE_DIRECTION_INPUT, AE_DIRECTION_OUTPUT}) {
    const uint32_t count =
        direction == AE_DIRECTION_INPUT ? caps.input_channels : caps.output_channels;
    for (uint32_t i = 0; i < count; ++i) {
      ae_channel_info channel{};
      channel.struct_size = sizeof(channel);
      require(ae_capability_snapshot_get_channel(snapshot, direction, i, &channel), "channel");
      std::printf("%s %u: %s native=%d supported=%u\n",
                  direction == AE_DIRECTION_INPUT ? "input" : "output", channel.index, channel.name,
                  channel.native_format, channel.supported);
    }
  }
  require(ae_capability_snapshot_release(snapshot), "release capabilities");
  if (run) {
    const uint32_t outputs[] = {0, 1};
    ae_stream_request request{};
    request.struct_size = sizeof(request);
    request.backend_id = backend.c_str();
    request.input_device_id = id.c_str();
    request.output_device_id = output_id.c_str();
    request.input_channels = {AE_CHANNEL_MODE_EXPLICIT, 1, &input};
    request.output_channels = {AE_CHANNEL_MODE_EXPLICIT, 2, outputs};
    request.sample_rate = rate;
    request.frames_per_block = frames;
    request.negotiation = AE_NEGOTIATION_EXACT;
    for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
      ae_stream *stream = nullptr;
      ae_stream_format format{};
      format.struct_size = sizeof(format);
      require(ae_stream_open(context, &request, &stream, &format), "open");
      std::printf("cycle=%u actual=%u/%u inputs=%u outputs=%u gain=%u%%\n", cycle + 1,
                  format.sample_rate, format.frames_per_block, format.input_channels,
                  format.output_channels, gain);
      Processor processor;
      processor.gain = static_cast<float>(gain) / 100;
      Tone generator;
      generator.start = format.sample_rate / 2;
      generator.step = 2.0 * 3.14159265358979323846 * 1000.0 / format.sample_rate;
      require(loop ? ae_stream_start(stream, tone, &generator)
                   : ae_stream_start(stream, process, &processor),
              "start");
      bool fault = false;
      for (uint32_t second = 0; second < seconds; ++second) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        print_status(stream, processor);
        ae_stream_status status{};
        status.struct_size = sizeof(status);
        require(ae_stream_get_status(stream, &status), "status");
        if (status.state != AE_STREAM_STATE_RUNNING) {
          fault = true;
          break;
        }
      }
      require(ae_stream_stop(stream), "stop");
      print_status(stream, processor);
      if (loop) {
        ae_stream_status status{};
        status.struct_size = sizeof(status);
        require(ae_stream_get_status(stream, &status), "status");
        const uint64_t detected = generator.detected.load();
        std::printf("tone start=%llu peak=%.6f reported_latency=%u\n",
                    static_cast<unsigned long long>(generator.start), generator.peak.load(),
                    status.input_latency_frames + status.output_latency_frames);
        if (detected == kUndetected) {
          std::printf("tone not detected on input %u\n", input);
          fault = true;
        } else {
          const uint64_t delay = detected - generator.start;
          std::printf("tone detected=%llu round_trip=%llu frames (%.3f ms)\n",
                      static_cast<unsigned long long>(detected),
                      static_cast<unsigned long long>(delay),
                      1000.0 * static_cast<double>(delay) / format.sample_rate);
        }
      }
      require(ae_stream_close(stream), "close");
      if (fault) {
        require(ae_context_destroy(context), "destroy context");
        return 1;
      }
    }
  }
  require(ae_context_destroy(context), "destroy context");
}
