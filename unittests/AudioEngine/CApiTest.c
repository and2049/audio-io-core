#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "audio_engine/engine.h"

_Static_assert(sizeof(ae_result) == 4, "ae_result must be 32-bit");
_Static_assert(sizeof(ae_channel_mode) == 4, "ae_channel_mode must be 32-bit");
_Static_assert(sizeof(ae_negotiation_policy) == 4, "ae_negotiation_policy must be 32-bit");
_Static_assert(sizeof(ae_stream_state) == 4, "ae_stream_state must be 32-bit");
_Static_assert(sizeof(ae_process_result) == 4, "ae_process_result must be 32-bit");
_Static_assert(offsetof(ae_stream_request, struct_size) == 0, "struct_size must be first");
_Static_assert(offsetof(ae_stream_format, struct_size) == 0, "struct_size must be first");
_Static_assert(offsetof(ae_audio_block, struct_size) == 0, "struct_size must be first");
_Static_assert(sizeof(((ae_audio_block *)0)->frame_position) == 8, "frame_position must be 64-bit");

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);                \
      return 1;                                                                                    \
    }                                                                                              \
  } while (0)

int main(void) {
  uint32_t major = 0;
  uint32_t minor = 0;
  CHECK(ae_get_api_version(&major, &minor) == AE_OK);
  CHECK(major == AE_API_VERSION_MAJOR);
  CHECK(minor == AE_API_VERSION_MINOR);

  ae_context_options options = {0};
  options.struct_size = (uint32_t)sizeof(ae_context_options);
  options.api_version_major = AE_API_VERSION_MAJOR;
  options.api_version_minor = AE_API_VERSION_MINOR;

  ae_context *context = NULL;
  CHECK(ae_context_create(&options, &context) == AE_OK);
  CHECK(context != NULL);

  ae_device_snapshot *snapshot = NULL;
  CHECK(ae_context_enumerate_devices(context, &snapshot) == AE_OK);
  ae_device_info info = {0};
  info.struct_size = sizeof(info);
  CHECK(ae_device_snapshot_get(snapshot, ae_device_snapshot_count(snapshot), &info) ==
        AE_ERROR_INVALID_ARGUMENT);
  CHECK(ae_device_snapshot_release(snapshot) == AE_OK);

  static const uint32_t channel = 0;
  ae_stream_request request = {0};
  request.struct_size = (uint32_t)sizeof(ae_stream_request);
  request.backend_id = "system";
  request.input_device_id = "default";
  request.output_device_id = "default";
  request.input_channels.mode = AE_CHANNEL_MODE_EXPLICIT;
  request.input_channels.count = 1;
  request.input_channels.indices = &channel;
  request.output_channels.mode = AE_CHANNEL_MODE_EXPLICIT;
  request.output_channels.count = 1;
  request.output_channels.indices = &channel;
  request.sample_rate = 48000;
  request.frames_per_block = 128;
  request.negotiation = AE_NEGOTIATION_EXACT;

  ae_stream *stream = NULL;
  ae_stream_format format = {0};
  format.struct_size = (uint32_t)sizeof(ae_stream_format);
  CHECK(ae_stream_open(context, &request, &stream, &format) == AE_ERROR_NOT_FOUND);
  CHECK(stream == NULL);
  CHECK(ae_context_destroy(context) == AE_OK);

  ae_context_options incompatible = options;
  incompatible.api_version_major = AE_API_VERSION_MAJOR + 1;
  context = NULL;
  CHECK(ae_context_create(&incompatible, &context) == AE_ERROR_INCOMPATIBLE_ABI);
  CHECK(context == NULL);

  return 0;
}
