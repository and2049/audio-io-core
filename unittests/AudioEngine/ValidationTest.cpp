#include <cstdint>
#include <cstring>
#include <memory>
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

TestContext makeTestContext() {
  auto backend = std::make_unique<TestBackend>();
  TestBackend *raw_backend = backend.get();
  ae_context *context = nullptr;
  AE_CHECK(ae::detail::create_context_with_backend(std::move(backend), &context) == AE_OK);
  return {context, raw_backend};
}

ae_context_options currentOptions() {
  ae_context_options options{};
  options.struct_size = static_cast<uint32_t>(sizeof(ae_context_options));
  options.api_version_major = AE_API_VERSION_MAJOR;
  options.api_version_minor = AE_API_VERSION_MINOR;
  return options;
}

ae_stream_format emptyFormat() {
  ae_stream_format format{};
  format.struct_size = static_cast<uint32_t>(sizeof(ae_stream_format));
  return format;
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

void testApiVersion() {
  uint32_t major = 0;
  uint32_t minor = 0;
  AE_CHECK(ae_get_api_version(&major, &minor) == AE_OK);
  AE_CHECK(major == AE_API_VERSION_MAJOR);
  AE_CHECK(minor == AE_API_VERSION_MINOR);
  AE_CHECK(ae_get_api_version(nullptr, &minor) == AE_ERROR_INVALID_ARGUMENT);
  AE_CHECK(ae_get_api_version(&major, nullptr) == AE_ERROR_INVALID_ARGUMENT);
}

void testProductionCatalog() {
  ae_context_options options = currentOptions();
  ae_context *context = nullptr;
  AE_CHECK(ae_context_create(&options, &context) == AE_OK);
  AE_CHECK(context != nullptr);

  ae_device_snapshot *snapshot = nullptr;
  AE_CHECK(ae_context_enumerate_devices(context, &snapshot) == AE_OK);
  AE_CHECK(snapshot != nullptr);
  const uint32_t count = ae_device_snapshot_count(snapshot);

  ae_device_info info{};
  info.struct_size = static_cast<uint32_t>(sizeof(ae_device_info));
  AE_CHECK(ae_device_snapshot_get(snapshot, count, &info) == AE_ERROR_INVALID_ARGUMENT);
  AE_CHECK(ae_device_snapshot_release(snapshot) == AE_OK);
  AE_CHECK(ae_context_destroy(context) == AE_OK);

  ae_stream_request request = duplexRequest();
  ae_stream *stream = nullptr;
  ae_stream_format format = emptyFormat();
  AE_CHECK(ae_context_create(&options, &context) == AE_OK);
  AE_CHECK(ae_stream_open(context, &request, &stream, &format) == AE_ERROR_NOT_FOUND);
  AE_CHECK(stream == nullptr);
  AE_CHECK(ae_context_destroy(context) == AE_OK);
}

void testIncompatibleAbi() {
  ae_context *context = nullptr;
  ae_context_options options = currentOptions();
  options.api_version_major = AE_API_VERSION_MAJOR + 1;
  AE_CHECK(ae_context_create(&options, &context) == AE_ERROR_INCOMPATIBLE_ABI);
  AE_CHECK(context == nullptr);

  options = currentOptions();
  options.api_version_minor = AE_API_VERSION_MINOR + 1;
  AE_CHECK(ae_context_create(&options, &context) == AE_ERROR_INCOMPATIBLE_ABI);
  AE_CHECK(context == nullptr);

  options = currentOptions();
  options.struct_size = static_cast<uint32_t>(sizeof(ae_context_options) - 1);
  AE_CHECK(ae_context_create(&options, &context) == AE_ERROR_INCOMPATIBLE_ABI);
  AE_CHECK(context == nullptr);

  AE_CHECK(ae_context_create(nullptr, &context) == AE_ERROR_INVALID_ARGUMENT);
  options = currentOptions();
  AE_CHECK(ae_context_create(&options, nullptr) == AE_ERROR_INVALID_ARGUMENT);
  AE_CHECK(ae_context_destroy(nullptr) == AE_ERROR_INVALID_ARGUMENT);
}

void expectOpenFailure(ae_context *context, const ae_stream_request &request, ae_result expected) {
  ae_stream *stream = nullptr;
  ae_stream_format format = emptyFormat();
  AE_CHECK(ae_stream_open(context, &request, &stream, &format) == expected);
  AE_CHECK(stream == nullptr);
}

void testRequestValidation() {
  TestContext test = makeTestContext();

  ae_stream_request request = duplexRequest();
  request.struct_size = static_cast<uint32_t>(sizeof(ae_stream_request) - 1);
  expectOpenFailure(test.context, request, AE_ERROR_INCOMPATIBLE_ABI);

  request = duplexRequest();
  request.backend_id = "";
  expectOpenFailure(test.context, request, AE_ERROR_INVALID_ARGUMENT);

  request = duplexRequest();
  request.input_device_id = nullptr;
  expectOpenFailure(test.context, request, AE_ERROR_INVALID_ARGUMENT);

  request = duplexRequest();
  request.output_device_id = "other-device";
  expectOpenFailure(test.context, request, AE_ERROR_UNSUPPORTED_CONFIGURATION);

  request = duplexRequest();
  request.input_device_id = "missing";
  request.output_device_id = "missing";
  expectOpenFailure(test.context, request, AE_ERROR_NOT_FOUND);

  request = duplexRequest();
  request.negotiation = 7;
  expectOpenFailure(test.context, request, AE_ERROR_INVALID_ARGUMENT);

  request = duplexRequest();
  request.sample_rate = 1;
  expectOpenFailure(test.context, request, AE_ERROR_INVALID_ARGUMENT);

  request = duplexRequest();
  request.frames_per_block = AE_MAX_FRAMES_PER_BLOCK + 1;
  expectOpenFailure(test.context, request, AE_ERROR_INVALID_ARGUMENT);

  request = duplexRequest();
  request.input_channels.mode = 9;
  expectOpenFailure(test.context, request, AE_ERROR_INVALID_ARGUMENT);

  request = duplexRequest();
  request.input_channels.mode = AE_CHANNEL_MODE_NONE;
  request.input_channels.count = 1;
  expectOpenFailure(test.context, request, AE_ERROR_INVALID_ARGUMENT);

  request = duplexRequest();
  request.input_channels.count = 0;
  expectOpenFailure(test.context, request, AE_ERROR_INVALID_ARGUMENT);

  request = duplexRequest();
  static constexpr uint32_t kDuplicateChannels[] = {1, 1};
  request.input_channels.count = 2;
  request.input_channels.indices = kDuplicateChannels;
  expectOpenFailure(test.context, request, AE_ERROR_INVALID_ARGUMENT);

  request = duplexRequest();
  static constexpr uint32_t kOutOfRangeChannels[] = {2};
  request.input_channels.indices = kOutOfRangeChannels;
  expectOpenFailure(test.context, request, AE_ERROR_INVALID_ARGUMENT);

  request = duplexRequest();
  request.input_channels.mode = AE_CHANNEL_MODE_NONE;
  request.input_channels.count = 0;
  request.input_channels.indices = nullptr;
  request.output_channels.mode = AE_CHANNEL_MODE_NONE;
  request.output_channels.count = 0;
  request.output_channels.indices = nullptr;
  expectOpenFailure(test.context, request, AE_ERROR_UNSUPPORTED_CONFIGURATION);

  AE_CHECK(ae_context_destroy(test.context) == AE_OK);
}

void testNegotiation() {
  TestContext test = makeTestContext();
  ae_stream *stream = nullptr;
  ae_stream_format format = emptyFormat();

  ae_stream_request request = duplexRequest();
  request.frames_per_block = 512;
  AE_CHECK(ae_stream_open(test.context, &request, &stream, &format) ==
           AE_ERROR_UNSUPPORTED_CONFIGURATION);
  AE_CHECK(stream == nullptr);

  request = duplexRequest();
  request.frames_per_block = 512;
  request.negotiation = AE_NEGOTIATION_ALLOW_ADJUSTMENT;
  format = emptyFormat();
  AE_CHECK(ae_stream_open(test.context, &request, &stream, &format) == AE_OK);
  AE_CHECK(format.sample_rate == 48000);
  AE_CHECK(format.frames_per_block == 128);
  AE_CHECK(ae_stream_close(stream) == AE_OK);

  request = duplexRequest();
  request.sample_rate = 32000;
  request.frames_per_block = 0;
  request.negotiation = AE_NEGOTIATION_ALLOW_ADJUSTMENT;
  format = emptyFormat();
  AE_CHECK(ae_stream_open(test.context, &request, &stream, &format) == AE_OK);
  AE_CHECK(format.sample_rate == 48000);
  AE_CHECK(ae_stream_close(stream) == AE_OK);

  request = duplexRequest();
  request.sample_rate = 44100;
  format = emptyFormat();
  AE_CHECK(ae_stream_open(test.context, &request, &stream, &format) == AE_OK);
  AE_CHECK(format.sample_rate == 44100);
  AE_CHECK(ae_stream_close(stream) == AE_OK);

  test.backend->setIgnoreExactNegotiation(true);
  request = duplexRequest();
  request.sample_rate = 44100;
  format = emptyFormat();
  AE_CHECK(ae_stream_open(test.context, &request, &stream, &format) ==
           AE_ERROR_UNSUPPORTED_CONFIGURATION);
  AE_CHECK(stream == nullptr);

  test.backend->setIgnoreExactNegotiation(false);
  format = emptyFormat();
  AE_CHECK(ae_stream_open(test.context, &request, &stream, &format) == AE_OK);
  AE_CHECK(format.sample_rate == 44100);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(ae_context_destroy(test.context) == AE_OK);
}

void testChannelSelections() {
  TestContext test = makeTestContext();
  ae_stream *stream = nullptr;

  ae_stream_request request = duplexRequest();
  request.input_channels.mode = AE_CHANNEL_MODE_BACKEND_DEFAULT;
  request.input_channels.count = 0;
  request.input_channels.indices = nullptr;
  request.output_channels.mode = AE_CHANNEL_MODE_BACKEND_DEFAULT;
  request.output_channels.count = 0;
  request.output_channels.indices = nullptr;
  ae_stream_format format = emptyFormat();
  AE_CHECK(ae_stream_open(test.context, &request, &stream, &format) == AE_OK);
  AE_CHECK(format.input_channels == 2);
  AE_CHECK(format.output_channels == 2);
  AE_CHECK(ae_stream_close(stream) == AE_OK);

  request = duplexRequest();
  request.input_channels.mode = AE_CHANNEL_MODE_NONE;
  request.input_channels.count = 0;
  request.input_channels.indices = nullptr;
  request.output_channels.count = 1;
  request.output_channels.indices = kOutputChannels;
  format = emptyFormat();
  AE_CHECK(ae_stream_open(test.context, &request, &stream, &format) == AE_OK);
  AE_CHECK(format.input_channels == 0);
  AE_CHECK(format.output_channels == 1);
  AE_CHECK(ae_stream_close(stream) == AE_OK);

  AE_CHECK(ae_context_destroy(test.context) == AE_OK);
}

void testCatalogSnapshotAndStreamOccupancy() {
  TestContext test = makeTestContext();
  ae_device_snapshot *first = nullptr;
  AE_CHECK(ae_context_enumerate_devices(test.context, &first) == AE_OK);
  AE_CHECK(ae_device_snapshot_count(first) == 1);

  ae_device_info info{};
  info.struct_size = static_cast<uint32_t>(sizeof(ae_device_info));
  AE_CHECK(ae_device_snapshot_get(first, 0, &info) == AE_OK);
  AE_CHECK(std::strcmp(info.backend_id, "test") == 0);
  AE_CHECK(std::strcmp(info.device_id, "test-duplex") == 0);
  AE_CHECK(std::strcmp(info.display_name, "Test Duplex") == 0);
  AE_CHECK(info.input_channels == 2);
  AE_CHECK(info.output_channels == 2);

  ae_device_info small{};
  small.struct_size = static_cast<uint32_t>(sizeof(uint32_t));
  AE_CHECK(ae_device_snapshot_get(first, 0, &small) == AE_ERROR_INCOMPATIBLE_ABI);
  AE_CHECK(ae_device_snapshot_get(first, 1, &info) == AE_ERROR_INVALID_ARGUMENT);

  ae_device_snapshot *second = nullptr;
  AE_CHECK(ae_context_enumerate_devices(test.context, &second) == AE_OK);
  AE_CHECK(ae_device_snapshot_count(second) == 1);
  info.struct_size = static_cast<uint32_t>(sizeof(ae_device_info));
  AE_CHECK(ae_device_snapshot_get(first, 0, &info) == AE_OK);
  AE_CHECK(std::strcmp(info.device_id, "test-duplex") == 0);

  AE_CHECK(ae_context_destroy(test.context) == AE_ERROR_BUSY);
  AE_CHECK(ae_device_snapshot_release(first) == AE_OK);
  AE_CHECK(ae_context_destroy(test.context) == AE_ERROR_BUSY);
  AE_CHECK(ae_device_snapshot_release(second) == AE_OK);

  ae_stream_request request = duplexRequest();
  ae_stream *stream = nullptr;
  ae_stream_format format = emptyFormat();
  AE_CHECK(ae_stream_open(test.context, &request, &stream, &format) == AE_OK);
  AE_CHECK(format.stream_id != 0);
  AE_CHECK(format.configuration_generation != 0);
  AE_CHECK(format.max_frames_per_block >= format.frames_per_block);

  ae_stream_format queried = emptyFormat();
  AE_CHECK(ae_stream_get_format(stream, &queried) == AE_OK);
  AE_CHECK(queried.stream_id == format.stream_id);
  AE_CHECK(queried.configuration_generation == format.configuration_generation);
  AE_CHECK(queried.sample_rate == format.sample_rate);
  AE_CHECK(queried.frames_per_block == format.frames_per_block);
  AE_CHECK(queried.input_channels == 1);
  AE_CHECK(queried.output_channels == 2);

  ae_stream_status status{};
  status.struct_size = static_cast<uint32_t>(sizeof(ae_stream_status));
  AE_CHECK(ae_stream_get_status(stream, &status) == AE_OK);
  AE_CHECK(status.state == AE_STREAM_STATE_READY);
  AE_CHECK(status.last_result == AE_OK);
  AE_CHECK(status.run_generation == 0);
  AE_CHECK(status.delivered_frames == 0);

  ae_stream *second_stream = nullptr;
  ae_stream_format second_format = emptyFormat();
  AE_CHECK(ae_stream_open(test.context, &request, &second_stream, &second_format) == AE_ERROR_BUSY);
  AE_CHECK(second_stream == nullptr);
  AE_CHECK(ae_context_destroy(test.context) == AE_ERROR_BUSY);

  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(ae_context_destroy(test.context) == AE_OK);
}

void testNullSnapshotArguments() {
  AE_CHECK(ae_device_snapshot_count(nullptr) == 0);
  AE_CHECK(ae_device_snapshot_release(nullptr) == AE_ERROR_INVALID_ARGUMENT);
  ae_device_info info{};
  info.struct_size = static_cast<uint32_t>(sizeof(ae_device_info));
  AE_CHECK(ae_device_snapshot_get(nullptr, 0, &info) == AE_ERROR_INVALID_ARGUMENT);
}

}

int main() {
  testApiVersion();
  testProductionCatalog();
  testIncompatibleAbi();
  testRequestValidation();
  testNegotiation();
  testChannelSelections();
  testCatalogSnapshotAndStreamOccupancy();
  testNullSnapshotArguments();
  return 0;
}
