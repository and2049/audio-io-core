#ifndef AUDIO_ENGINE_ENGINE_H
#define AUDIO_ENGINE_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AE_API_VERSION_MAJOR 0u
#define AE_API_VERSION_MINOR 1u
#define AE_MAX_SAMPLE_RATE 768000u
#define AE_MAX_FRAMES_PER_BLOCK 65536u

typedef int32_t ae_result;
enum {
  AE_OK = 0,
  AE_ERROR_INVALID_ARGUMENT = 1,
  AE_ERROR_INCOMPATIBLE_ABI = 2,
  AE_ERROR_INVALID_STATE = 3,
  AE_ERROR_NOT_FOUND = 4,
  AE_ERROR_BUSY = 5,
  AE_ERROR_UNSUPPORTED = 6,
  AE_ERROR_UNSUPPORTED_CONFIGURATION = 7,
  AE_ERROR_DEVICE_UNAVAILABLE = 8,
  AE_ERROR_DRIVER_FAILURE = 9,
  AE_ERROR_PROCESSOR_FAILURE = 10,
  AE_ERROR_ALLOCATION_FAILURE = 11,
  AE_ERROR_QUIESCENCE_FAILURE = 12
};

typedef int32_t ae_channel_mode;
enum {
  AE_CHANNEL_MODE_NONE = 0,
  AE_CHANNEL_MODE_BACKEND_DEFAULT = 1,
  AE_CHANNEL_MODE_EXPLICIT = 2
};

typedef int32_t ae_negotiation_policy;
enum { AE_NEGOTIATION_EXACT = 0, AE_NEGOTIATION_ALLOW_ADJUSTMENT = 1 };

typedef int32_t ae_stream_state;
enum {
  AE_STREAM_STATE_READY = 0,
  AE_STREAM_STATE_RUNNING = 1,
  AE_STREAM_STATE_RECOVERY_REQUIRED = 2
};

typedef int32_t ae_process_result;
enum { AE_PROCESS_OK = 0, AE_PROCESS_SILENCE = 1, AE_PROCESS_FAILED = 2 };

typedef struct ae_context ae_context;
typedef struct ae_stream ae_stream;
typedef struct ae_device_snapshot ae_device_snapshot;

typedef struct ae_context_options {
  uint32_t struct_size;
  uint32_t api_version_major;
  uint32_t api_version_minor;
  uint32_t reserved;
} ae_context_options;

typedef struct ae_device_info {
  uint32_t struct_size;
  const char *backend_id;
  const char *device_id;
  const char *display_name;
  uint32_t input_channels;
  uint32_t output_channels;
} ae_device_info;

typedef struct ae_channel_selection {
  ae_channel_mode mode;
  uint32_t count;
  const uint32_t *indices;
} ae_channel_selection;

typedef struct ae_stream_request {
  uint32_t struct_size;
  const char *backend_id;
  const char *input_device_id;
  const char *output_device_id;
  ae_channel_selection input_channels;
  ae_channel_selection output_channels;
  uint32_t sample_rate;
  uint32_t frames_per_block;
  ae_negotiation_policy negotiation;
} ae_stream_request;

typedef struct ae_stream_format {
  uint32_t struct_size;
  uint32_t sample_rate;
  uint32_t frames_per_block;
  uint32_t max_frames_per_block;
  uint32_t input_channels;
  uint32_t output_channels;
  uint32_t stream_id;
  uint32_t configuration_generation;
} ae_stream_format;

typedef struct ae_audio_block {
  uint32_t struct_size;
  const float *const *inputs;
  uint32_t input_channels;
  float *const *outputs;
  uint32_t output_channels;
  uint32_t frames;
  uint32_t stream_id;
  uint32_t configuration_generation;
  uint32_t run_generation;
  uint64_t frame_position;
} ae_audio_block;

typedef ae_process_result (*ae_process_fn)(void *user, const ae_audio_block *block);

typedef struct ae_stream_status {
  uint32_t struct_size;
  ae_stream_state state;
  ae_result last_result;
  uint32_t stream_id;
  uint32_t configuration_generation;
  uint32_t run_generation;
  uint64_t delivered_frames;
} ae_stream_status;

ae_result ae_get_api_version(uint32_t *out_major, uint32_t *out_minor);
ae_result ae_context_create(const ae_context_options *options, ae_context **out_context);
ae_result ae_context_destroy(ae_context *context);
ae_result ae_context_enumerate_devices(const ae_context *context,
                                       ae_device_snapshot **out_snapshot);
uint32_t ae_device_snapshot_count(const ae_device_snapshot *snapshot);
ae_result ae_device_snapshot_get(const ae_device_snapshot *snapshot, uint32_t index,
                                 ae_device_info *out_info);
ae_result ae_device_snapshot_release(ae_device_snapshot *snapshot);
ae_result ae_stream_open(ae_context *context, const ae_stream_request *request,
                         ae_stream **out_stream, ae_stream_format *out_format);
ae_result ae_stream_get_format(const ae_stream *stream, ae_stream_format *out_format);
ae_result ae_stream_start(ae_stream *stream, ae_process_fn process, void *user);
ae_result ae_stream_stop(ae_stream *stream);
ae_result ae_stream_get_status(const ae_stream *stream, ae_stream_status *out_status);
ae_result ae_stream_close(ae_stream *stream);

#ifdef __cplusplus
}
#endif

#endif
