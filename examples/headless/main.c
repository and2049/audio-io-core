#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "audio_engine/engine.h"

int main(void) {
  uint32_t major = 0;
  uint32_t minor = 0;
  if (ae_get_api_version(&major, &minor) != AE_OK) {
    fprintf(stderr, "failed to query API version\n");
    return 1;
  }
  printf("audio_engine API %" PRIu32 ".%" PRIu32 "\n", major, minor);

  ae_context_options options = {0};
  options.struct_size = (uint32_t)sizeof(ae_context_options);
  options.api_version_major = AE_API_VERSION_MAJOR;
  options.api_version_minor = AE_API_VERSION_MINOR;

  ae_context *context = NULL;
  ae_result result = ae_context_create(&options, &context);
  if (result != AE_OK) {
    fprintf(stderr, "context creation failed: %" PRId32 "\n", result);
    return 1;
  }

  ae_device_snapshot *snapshot = NULL;
  result = ae_context_enumerate_devices(context, &snapshot);
  if (result != AE_OK) {
    fprintf(stderr, "device enumeration failed: %" PRId32 "\n", result);
    ae_context_destroy(context);
    return 1;
  }

  const uint32_t count = ae_device_snapshot_count(snapshot);
  printf("devices: %" PRIu32 "\n", count);
  for (uint32_t index = 0; index < count; ++index) {
    ae_device_info info = {0};
    info.struct_size = (uint32_t)sizeof(ae_device_info);
    if (ae_device_snapshot_get(snapshot, index, &info) != AE_OK) {
      fprintf(stderr, "device %" PRIu32 " unavailable\n", index);
      ae_device_snapshot_release(snapshot);
      ae_context_destroy(context);
      return 1;
    }
    printf("  [%" PRIu32 "] %s (%s/%s) in:%" PRIu32 " out:%" PRIu32 "\n", index, info.display_name,
           info.backend_id, info.device_id, info.input_channels, info.output_channels);
  }
  if (count == 0) {
    printf("no devices: this build contains no audio backend\n");
  }

  ae_device_snapshot_release(snapshot);
  ae_context_destroy(context);
  return 0;
}
