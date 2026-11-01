#include <memory>

#include "Check.h"
#include "TestBackend.h"
#include "testing_api.h"

namespace {
ae_process_result silence(void *, const ae_audio_block *) { return AE_PROCESS_SILENCE; }
}

int main() {
  auto backend = std::make_unique<TestBackend>();
  auto *driver = backend.get();
  ae_context *context = nullptr;
  AE_CHECK(ae::detail::create_context_with_backend(std::move(backend), &context) == AE_OK);
  ae_capability_snapshot *snapshot = nullptr;
  AE_CHECK(ae_context_probe_device(context, "test", "missing", &snapshot) == AE_ERROR_NOT_FOUND);
  AE_CHECK(snapshot == nullptr);
  AE_CHECK(ae_context_probe_device(context, "test", "test-duplex", &snapshot) == AE_OK);
  AE_CHECK(ae_context_destroy(context) == AE_ERROR_BUSY);
  ae_device_capabilities caps{};
  caps.struct_size = sizeof(caps);
  AE_CHECK(ae_capability_snapshot_get(snapshot, &caps) == AE_OK);
  AE_CHECK(caps.input_channels == 2 && caps.tested_rate_count == 3);
  AE_CHECK(caps.tested_rates[1] == 48000 && caps.frame_granularity == -1);
  AE_CHECK(ae_context_refresh_devices(context) == AE_OK);
  AE_CHECK(caps.tested_rates[2] == 96000);
  ae_channel_info channel{};
  channel.struct_size = sizeof(channel);
  AE_CHECK(ae_capability_snapshot_get_channel(snapshot, AE_DIRECTION_INPUT, 1, &channel) == AE_OK);
  AE_CHECK(channel.index == 1);
  AE_CHECK(ae_capability_snapshot_get_channel(snapshot, 7, 0, &channel) ==
           AE_ERROR_INVALID_ARGUMENT);
  AE_CHECK(ae_capability_snapshot_get_channel(snapshot, AE_DIRECTION_INPUT, 2, &channel) ==
           AE_ERROR_INVALID_ARGUMENT);
  AE_CHECK(ae_capability_snapshot_release(snapshot) == AE_OK);

  const uint32_t map[] = {1, 0};
  ae_stream_request request{};
  request.struct_size = sizeof(request);
  request.backend_id = "test";
  request.input_device_id = "test-duplex";
  request.output_device_id = "test-duplex";
  request.input_channels = {AE_CHANNEL_MODE_EXPLICIT, 2, map};
  request.output_channels = {AE_CHANNEL_MODE_BACKEND_DEFAULT, 0, nullptr};
  ae_stream_format format{};
  format.struct_size = sizeof(format);
  ae_stream *stream = nullptr;
  AE_CHECK(ae_stream_open(context, &request, &stream, &format) == AE_OK);
  uint32_t physical = 0;
  AE_CHECK(ae_stream_get_channel(stream, AE_DIRECTION_INPUT, 0, &physical) == AE_OK);
  AE_CHECK(physical == 1);
  AE_CHECK(ae_context_probe_device(context, "test", "test-duplex", &snapshot) == AE_ERROR_BUSY);
  driver->setRetainedStartFailure();
  AE_CHECK(ae_stream_start(stream, silence, nullptr) == AE_ERROR_QUIESCENCE_FAILURE);
  ae_stream_status status{};
  status.struct_size = sizeof(status);
  AE_CHECK(ae_stream_get_status(stream, &status) == AE_OK);
  AE_CHECK(status.state == AE_STREAM_STATE_RECOVERY_REQUIRED);
  AE_CHECK(ae_stream_stop(stream) == AE_OK);
  driver->invalidate();
  AE_CHECK(ae_stream_get_status(stream, &status) == AE_OK);
  AE_CHECK(status.state == AE_STREAM_STATE_RECOVERY_REQUIRED &&
           status.fault_flags == AE_FAULT_RESET);
  AE_CHECK(ae_stream_start(stream, silence, nullptr) == AE_ERROR_INVALID_STATE);
  AE_CHECK(ae_stream_stop(stream) == AE_OK);
  AE_CHECK(ae_stream_start(stream, silence, nullptr) == AE_ERROR_INVALID_STATE);
  driver->setCloseFailure();
  AE_CHECK(ae_stream_close(stream) == AE_ERROR_DRIVER_FAILURE);
  AE_CHECK(ae_context_destroy(context) == AE_ERROR_BUSY);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  driver->setCloseFailure();
  AE_CHECK(ae_context_destroy(context) == AE_ERROR_DRIVER_FAILURE);
  AE_CHECK(ae_context_destroy(context) == AE_OK);
}
