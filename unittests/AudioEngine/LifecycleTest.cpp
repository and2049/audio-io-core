#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <utility>

#include "Check.h"
#include "TestBackend.h"
#include "audio_engine/engine.h"
#include "testing_api.h"

namespace {

constexpr uint32_t kInputChannels[] = {1};
constexpr uint32_t kOutputChannels[] = {0, 1};

struct TestContext {
  ae_context *context = nullptr;
  TestBackend *backend = nullptr;
};

struct Recorder {
  std::atomic<uint32_t> calls{0};
  std::atomic<uint32_t> active{0};
  std::atomic<bool> invalid{false};
  std::atomic<bool> fail{false};
  std::atomic<uint64_t> next_position{0};
  std::atomic<uint32_t> expected_frames{0};
  std::atomic<uint32_t> expected_inputs{0};
  std::atomic<uint32_t> expected_outputs{0};
  std::atomic<uint32_t> expected_stream{0};
  std::atomic<uint32_t> expected_configuration{0};
  std::atomic<uint32_t> expected_run{0};
};

TestContext makeTestContext() {
  auto backend = std::make_unique<TestBackend>();
  TestBackend *raw_backend = backend.get();
  ae_context *context = nullptr;
  AE_CHECK(ae::detail::create_context_with_backend(std::move(backend), &context) == AE_OK);
  return {context, raw_backend};
}

ae_stream_format emptyFormat() {
  ae_stream_format format{};
  format.struct_size = static_cast<uint32_t>(sizeof(ae_stream_format));
  return format;
}

ae_stream_status emptyStatus() {
  ae_stream_status status{};
  status.struct_size = static_cast<uint32_t>(sizeof(ae_stream_status));
  return status;
}

ae_stream_request duplexRequest() {
  ae_stream_request request{};
  request.struct_size = static_cast<uint32_t>(sizeof(ae_stream_request));
  request.backend_id = "test";
  request.input_device_id = "test-duplex";
  request.output_device_id = "test-duplex";
  request.input_channels.mode = AE_CHANNEL_MODE_EXPLICIT;
  request.input_channels.count = 1;
  request.input_channels.indices = kInputChannels;
  request.output_channels.mode = AE_CHANNEL_MODE_EXPLICIT;
  request.output_channels.count = 2;
  request.output_channels.indices = kOutputChannels;
  request.sample_rate = 48000;
  request.frames_per_block = 128;
  request.negotiation = AE_NEGOTIATION_EXACT;
  return request;
}

void configureRecorder(Recorder &recorder, const ae_stream_format &format,
                       uint32_t run_generation) {
  recorder.expected_frames = format.frames_per_block;
  recorder.expected_inputs = format.input_channels;
  recorder.expected_outputs = format.output_channels;
  recorder.expected_stream = format.stream_id;
  recorder.expected_configuration = format.configuration_generation;
  recorder.expected_run = run_generation;
}

ae_process_result process(void *user, const ae_audio_block *block) {
  auto *recorder = static_cast<Recorder *>(user);
  recorder->calls.fetch_add(1, std::memory_order_relaxed);
  recorder->active.fetch_add(1, std::memory_order_acq_rel);

  if (recorder->fail.load(std::memory_order_relaxed)) {
    recorder->active.fetch_sub(1, std::memory_order_acq_rel);
    return AE_PROCESS_FAILED;
  }

  bool valid = block != nullptr;
  if (valid) {
    valid = valid && block->struct_size >= sizeof(ae_audio_block);
    valid = valid && block->frames == recorder->expected_frames.load();
    valid = valid && block->input_channels == recorder->expected_inputs.load();
    valid = valid && block->output_channels == recorder->expected_outputs.load();
    valid = valid && block->stream_id == recorder->expected_stream.load();
    valid = valid && block->configuration_generation == recorder->expected_configuration.load();
    valid = valid && block->run_generation == recorder->expected_run.load();
    valid = valid && block->frame_position == recorder->next_position.load();
  }
  if (valid && block->input_channels > 0) {
    valid = block->inputs != nullptr;
  }
  if (valid && block->output_channels > 0) {
    valid = block->outputs != nullptr;
  }
  if (valid) {
    for (uint32_t channel = 0; channel < block->input_channels; ++channel) {
      valid = valid && block->inputs[channel] != nullptr;
    }
    for (uint32_t channel = 0; channel < block->output_channels; ++channel) {
      valid = valid && block->outputs[channel] != nullptr;
      valid = valid && block->outputs[channel][0] == 0.0f;
      valid = valid && block->outputs[channel][block->frames - 1] == 0.0f;
    }
    for (uint32_t input = 0; input < block->input_channels; ++input) {
      for (uint32_t output = 0; output < block->output_channels; ++output) {
        valid = valid && block->inputs[input] != block->outputs[output];
      }
    }
  }

  if (valid) {
    const float value = block->input_channels > 0 ? block->inputs[0][0] : 0.25f;
    for (uint32_t channel = 0; channel < block->output_channels; ++channel) {
      std::fill_n(block->outputs[channel], block->frames, value);
    }
    recorder->next_position.store(block->frame_position + block->frames);
  } else {
    recorder->invalid.store(true, std::memory_order_relaxed);
  }

  recorder->active.fetch_sub(1, std::memory_order_acq_rel);
  return AE_OK;
}

void waitForCalls(Recorder &recorder, uint32_t count) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (recorder.calls.load(std::memory_order_acquire) < count) {
    if (std::chrono::steady_clock::now() > deadline) {
      std::fprintf(stderr, "timed out waiting for %u callbacks\n", count);
      std::exit(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void waitForDelivered(ae_stream *stream, uint64_t frames) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (true) {
    ae_stream_status status = emptyStatus();
    AE_CHECK(ae_stream_get_status(stream, &status) == AE_OK);
    if (status.delivered_frames >= frames) {
      return;
    }
    if (std::chrono::steady_clock::now() > deadline) {
      std::fprintf(stderr, "timed out waiting for %llu delivered frames (%llu delivered)\n",
                   static_cast<unsigned long long>(frames),
                   static_cast<unsigned long long>(status.delivered_frames));
      std::exit(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void waitForState(ae_stream *stream, ae_stream_state expected) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (true) {
    ae_stream_status status = emptyStatus();
    AE_CHECK(ae_stream_get_status(stream, &status) == AE_OK);
    if (status.state == expected) {
      return;
    }
    if (std::chrono::steady_clock::now() > deadline) {
      std::fprintf(stderr, "timed out waiting for stream state %d\n", expected);
      std::exit(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void testLifecycleAndQuiescence() {
  TestContext test = makeTestContext();
  ae_stream_request request = duplexRequest();
  ae_stream *stream = nullptr;
  ae_stream_format format = emptyFormat();
  AE_CHECK(ae_stream_open(test.context, &request, &stream, &format) == AE_OK);
  AE_CHECK(format.sample_rate == 48000);
  AE_CHECK(format.frames_per_block == 128);
  AE_CHECK(format.input_channels == 1);
  AE_CHECK(format.output_channels == 2);

  Recorder recorder;
  configureRecorder(recorder, format, 1);
  AE_CHECK(ae_stream_start(stream, process, &recorder) == AE_OK);
  waitForCalls(recorder, 5);
  waitForDelivered(stream, static_cast<uint64_t>(5 * 128));

  ae_stream_status status = emptyStatus();
  AE_CHECK(ae_stream_get_status(stream, &status) == AE_OK);
  AE_CHECK(status.state == AE_STREAM_STATE_RUNNING);
  AE_CHECK(status.run_generation == 1);
  AE_CHECK(status.last_result == AE_OK);
  AE_CHECK(status.delivered_frames >= static_cast<uint64_t>(5 * 128));
  AE_CHECK(status.delivered_frames % 128 == 0);

  AE_CHECK(ae_stream_start(stream, process, &recorder) == AE_ERROR_INVALID_STATE);

  AE_CHECK(ae_stream_stop(stream) == AE_OK);
  const uint32_t calls_at_stop = recorder.calls.load(std::memory_order_acquire);
  AE_CHECK(recorder.active.load(std::memory_order_acquire) == 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  AE_CHECK(recorder.calls.load(std::memory_order_acquire) == calls_at_stop);
  AE_CHECK(!recorder.invalid.load(std::memory_order_acquire));

  status = emptyStatus();
  AE_CHECK(ae_stream_get_status(stream, &status) == AE_OK);
  AE_CHECK(status.state == AE_STREAM_STATE_READY);
  AE_CHECK(status.run_generation == 1);
  AE_CHECK(status.last_result == AE_OK);

  recorder.expected_run = 2;
  recorder.next_position = 0;
  AE_CHECK(ae_stream_start(stream, process, &recorder) == AE_OK);
  waitForCalls(recorder, calls_at_stop + 5);
  status = emptyStatus();
  AE_CHECK(ae_stream_get_status(stream, &status) == AE_OK);
  AE_CHECK(status.state == AE_STREAM_STATE_RUNNING);
  AE_CHECK(status.run_generation == 2);
  AE_CHECK(status.delivered_frames % 128 == 0);
  AE_CHECK(ae_stream_stop(stream) == AE_OK);

  const uint32_t calls_before_close = recorder.calls.load(std::memory_order_acquire);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  AE_CHECK(recorder.calls.load(std::memory_order_acquire) == calls_before_close);
  AE_CHECK(!recorder.invalid.load(std::memory_order_acquire));
  AE_CHECK(ae_context_destroy(test.context) == AE_OK);
}

void testProcessorFailureLatches() {
  TestContext test = makeTestContext();
  ae_stream_request request = duplexRequest();
  ae_stream *stream = nullptr;
  ae_stream_format format = emptyFormat();
  AE_CHECK(ae_stream_open(test.context, &request, &stream, &format) == AE_OK);

  Recorder recorder;
  configureRecorder(recorder, format, 1);
  recorder.fail = true;
  AE_CHECK(ae_stream_start(stream, process, &recorder) == AE_OK);
  waitForState(stream, AE_STREAM_STATE_RECOVERY_REQUIRED);

  AE_CHECK(recorder.calls.load(std::memory_order_acquire) == 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  AE_CHECK(recorder.calls.load(std::memory_order_acquire) == 1);

  ae_stream_status status = emptyStatus();
  AE_CHECK(ae_stream_get_status(stream, &status) == AE_OK);
  AE_CHECK(status.state == AE_STREAM_STATE_RECOVERY_REQUIRED);
  AE_CHECK(status.last_result == AE_ERROR_PROCESSOR_FAILURE);
  AE_CHECK(status.delivered_frames == 0);

  AE_CHECK(ae_stream_start(stream, process, &recorder) == AE_ERROR_INVALID_STATE);
  AE_CHECK(ae_stream_stop(stream) == AE_OK);
  status = emptyStatus();
  AE_CHECK(ae_stream_get_status(stream, &status) == AE_OK);
  AE_CHECK(status.state == AE_STREAM_STATE_READY);
  AE_CHECK(status.last_result == AE_ERROR_PROCESSOR_FAILURE);
  AE_CHECK(recorder.calls.load(std::memory_order_acquire) == 1);

  recorder.fail = false;
  recorder.expected_run = 2;
  recorder.next_position = 0;
  AE_CHECK(ae_stream_start(stream, process, &recorder) == AE_OK);
  waitForCalls(recorder, 2);
  AE_CHECK(ae_stream_stop(stream) == AE_OK);
  AE_CHECK(!recorder.invalid.load(std::memory_order_acquire));
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(ae_context_destroy(test.context) == AE_OK);
}

void testFailedStartKeepsReady() {
  TestContext test = makeTestContext();
  ae_stream_request request = duplexRequest();
  ae_stream *stream = nullptr;
  ae_stream_format format = emptyFormat();
  AE_CHECK(ae_stream_open(test.context, &request, &stream, &format) == AE_OK);

  Recorder recorder;
  configureRecorder(recorder, format, 1);
  test.backend->setFailNextStart(true);
  AE_CHECK(ae_stream_start(stream, process, &recorder) == AE_ERROR_DRIVER_FAILURE);

  ae_stream_status status = emptyStatus();
  AE_CHECK(ae_stream_get_status(stream, &status) == AE_OK);
  AE_CHECK(status.state == AE_STREAM_STATE_READY);
  AE_CHECK(status.run_generation == 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  AE_CHECK(recorder.calls.load(std::memory_order_acquire) == 0);

  AE_CHECK(ae_stream_start(stream, process, &recorder) == AE_OK);
  waitForCalls(recorder, 2);
  AE_CHECK(ae_stream_stop(stream) == AE_OK);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(ae_context_destroy(test.context) == AE_OK);
}

void testCloseStopsRunningStream() {
  TestContext test = makeTestContext();
  ae_stream_request request = duplexRequest();
  ae_stream *stream = nullptr;
  ae_stream_format format = emptyFormat();
  AE_CHECK(ae_stream_open(test.context, &request, &stream, &format) == AE_OK);

  Recorder recorder;
  configureRecorder(recorder, format, 1);
  AE_CHECK(ae_stream_start(stream, process, &recorder) == AE_OK);
  waitForCalls(recorder, 5);

  AE_CHECK(ae_stream_close(stream) == AE_OK);
  const uint32_t calls_at_close = recorder.calls.load(std::memory_order_acquire);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  AE_CHECK(recorder.calls.load(std::memory_order_acquire) == calls_at_close);
  AE_CHECK(recorder.active.load(std::memory_order_acquire) == 0);
  AE_CHECK(!recorder.invalid.load(std::memory_order_acquire));
  AE_CHECK(ae_context_destroy(test.context) == AE_OK);
}

void testOutputOnlyCallback() {
  TestContext test = makeTestContext();
  ae_stream_request request = duplexRequest();
  request.input_channels.mode = AE_CHANNEL_MODE_NONE;
  request.input_channels.count = 0;
  request.input_channels.indices = nullptr;
  ae_stream *stream = nullptr;
  ae_stream_format format = emptyFormat();
  AE_CHECK(ae_stream_open(test.context, &request, &stream, &format) == AE_OK);
  AE_CHECK(format.input_channels == 0);
  AE_CHECK(format.output_channels == 2);

  Recorder recorder;
  configureRecorder(recorder, format, 1);
  AE_CHECK(ae_stream_start(stream, process, &recorder) == AE_OK);
  waitForCalls(recorder, 3);
  AE_CHECK(ae_stream_stop(stream) == AE_OK);
  AE_CHECK(!recorder.invalid.load(std::memory_order_acquire));
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(ae_context_destroy(test.context) == AE_OK);
}

}

int main() {
  testLifecycleAndQuiescence();
  testProcessorFailureLatches();
  testFailedStartKeepsReady();
  testCloseStopsRunningStream();
  testOutputOnlyCallback();
  return 0;
}
