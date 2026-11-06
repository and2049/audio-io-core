#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <map>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "Check.h"
#include "backends/darwin/coreaudio_backend.h"
#include "callback_timing.h"
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

namespace {
constexpr const char *duplex = "fake-duplex";
constexpr const char *mixed = "fake-mixed";
constexpr const char *playback = "fake-playback";
constexpr AudioObjectID kDuplex = 10, kMixed = 30, kPlayback = 20, kEmpty = 40, kStale = 50;
constexpr AudioObjectID kAggregate = 90;
constexpr float kGarbage = 9.0f;

float sample(uint32_t channel, uint32_t frame) {
  return static_cast<float>(channel + 1) * 0.125f + static_cast<float>(frame) / 4096.0f;
}

std::string utf8(CFStringRef value) {
  char text[256]{};
  AE_CHECK(CFStringGetCString(value, text, sizeof(text), kCFStringEncodingUTF8));
  return text;
}

struct Stream {
  AudioObjectID id = 0;
  uint32_t channels = 0;
  UInt32 latency = 0;
  bool floating = true;
};

struct Device {
  AudioObjectID id = 0;
  std::string uid, name;
  std::vector<Stream> streams[2];
  Float64 rate = 48000;
  UInt32 frames = 512;
  AudioValueRange range{16, 1024};
  std::vector<AudioValueRange> rates{{44100, 44100}, {48000, 48000}, {96000, 96000}};
  UInt32 latency[2]{10, 30}, safety[2]{20, 40};
  UInt32 alive = 1, variable = 0;
  std::map<UInt32, std::string> names[2];
};

struct Buffers {
  std::vector<uint64_t> storage;
  std::vector<std::vector<float>> data;
  AudioBufferList *list() { return reinterpret_cast<AudioBufferList *>(storage.data()); }
};

class Hal final : public ae::CoreAudioHal {
public:
  Hal() { reset(); }

  void reset() {
    devices.clear();
    creations = destructions = starts = rate_sets = frame_sets = 0;
    aggregates_created = aggregates_destroyed = access_queries = 0;
    Device first;
    first.id = kDuplex;
    first.uid = duplex;
    first.name = "Fake Duplex";
    first.streams[0] = {{11, 2, 5}, {12, 1, 6}};
    first.streams[1] = {{13, 1, 7}, {14, 2, 8}};
    first.names[0][1] = "Mic";
    devices.push_back(first);
    Device second;
    second.id = kPlayback;
    second.uid = playback;
    second.name = "Fake Playback";
    second.streams[1] = {{21, 2, 3}};
    devices.push_back(second);
    Device third;
    third.id = kMixed;
    third.uid = mixed;
    third.name = "Fake Mixed";
    third.streams[0] = {{31, 1, 2}};
    third.streams[1] = {{32, 2, 4}};
    devices.push_back(third);
    Device empty;
    empty.id = kEmpty;
    empty.uid = "fake-empty";
    devices.push_back(empty);
    Device stale;
    stale.id = kStale;
    stale.uid = "audio-io-core.aggregate.1.1";
    stale.streams[1] = {{51, 2, 0}};
    devices.push_back(stale);
  }

  Device *find(AudioObjectID id) {
    for (Device &device : devices) {
      if (device.id == id) {
        return &device;
      }
    }
    return nullptr;
  }
  const Stream *stream(AudioObjectID id) {
    for (Device &device : devices) {
      for (auto &side : device.streams) {
        for (Stream &entry : side) {
          if (entry.id == id) {
            return &entry;
          }
        }
      }
    }
    return nullptr;
  }

  std::vector<uint8_t> value(AudioObjectID object, const AudioObjectPropertyAddress &address,
                             OSStatus &status) {
    status = noErr;
    auto bytes = [](const auto &item) {
      const auto *begin = reinterpret_cast<const uint8_t *>(&item);
      return std::vector<uint8_t>(begin, begin + sizeof(item));
    };
    auto array = [](const auto &items) {
      const auto *begin = reinterpret_cast<const uint8_t *>(items.data());
      return std::vector<uint8_t>(begin, begin + items.size() * sizeof(items[0]));
    };
    auto text = [&](const std::string &content) {
      return bytes(CFStringCreateWithCString(nullptr, content.c_str(), kCFStringEncodingUTF8));
    };
    const int direction = address.mScope == kAudioObjectPropertyScopeInput ? 0 : 1;
    if (object == kAudioObjectSystemObject) {
      if (address.mSelector == kAudioHardwarePropertyDevices) {
        if (fail_devices) {
          status = kAudioHardwareUnspecifiedError;
          return {};
        }
        std::vector<AudioObjectID> ids;
        for (const Device &device : devices) {
          ids.push_back(device.id);
        }
        return array(ids);
      }
    } else if (const Stream *entry = stream(object)) {
      AudioStreamBasicDescription format{};
      format.mFormatID = kAudioFormatLinearPCM;
      format.mChannelsPerFrame = entry->channels;
      switch (address.mSelector) {
      case kAudioStreamPropertyVirtualFormat:
        format.mFormatFlags = entry->floating ? kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked
                                              : kAudioFormatFlagIsSignedInteger;
        format.mBitsPerChannel = 32;
        format.mBytesPerFrame = 4 * entry->channels;
        return bytes(format);
      case kAudioStreamPropertyPhysicalFormat:
        format.mFormatFlags = kAudioFormatFlagIsSignedInteger;
        format.mBitsPerChannel = 24;
        format.mBytesPerFrame = 3 * entry->channels;
        return bytes(format);
      case kAudioStreamPropertyLatency:
        return bytes(entry->latency);
      default:
        break;
      }
    } else if (Device *device = find(object)) {
      switch (address.mSelector) {
      case kAudioDevicePropertyDeviceUID:
        return text(device->uid);
      case kAudioObjectPropertyName:
        return text(device->name);
      case kAudioObjectPropertyElementName:
        if (device->names[direction].count(address.mElement) != 0) {
          return text(device->names[direction][address.mElement]);
        }
        break;
      case kAudioDevicePropertyStreamConfiguration: {
        const auto &streams = device->streams[direction];
        std::vector<uint8_t> result(offsetof(AudioBufferList, mBuffers) +
                                    streams.size() * sizeof(AudioBuffer));
        std::vector<uint64_t> aligned((result.size() + 7) / 8);
        auto *list = reinterpret_cast<AudioBufferList *>(aligned.data());
        list->mNumberBuffers = static_cast<UInt32>(streams.size());
        for (size_t i = 0; i < streams.size(); ++i) {
          list->mBuffers[i].mNumberChannels = streams[i].channels;
        }
        std::memcpy(result.data(), aligned.data(), result.size());
        return result;
      }
      case kAudioDevicePropertyStreams: {
        std::vector<AudioObjectID> ids;
        for (const Stream &item : device->streams[direction]) {
          ids.push_back(item.id);
        }
        if (drop_stream && !ids.empty()) {
          ids.pop_back();
        }
        return array(ids);
      }
      case kAudioDevicePropertyBufferFrameSizeRange:
        return bytes(device->range);
      case kAudioDevicePropertyBufferFrameSize:
        return bytes(device->frames);
      case kAudioDevicePropertyNominalSampleRate:
        return bytes(device->rate);
      case kAudioDevicePropertyAvailableNominalSampleRates:
        return array(device->rates);
      case kAudioDevicePropertyLatency:
        if (fail_latency) {
          break;
        }
        return bytes(device->latency[direction]);
      case kAudioDevicePropertySafetyOffset:
        return bytes(device->safety[direction]);
      case kAudioDevicePropertyDeviceIsAlive:
        return bytes(device->alive);
      case kAudioDevicePropertyUsesVariableBufferFrameSizes:
        if (device->variable != 0) {
          return bytes(device->variable);
        }
        break;
      default:
        break;
      }
    } else {
      status = kAudioHardwareBadObjectError;
      return {};
    }
    status = kAudioHardwareUnknownPropertyError;
    return {};
  }

  OSStatus size(AudioObjectID object, const AudioObjectPropertyAddress &address,
                UInt32 &bytes) override {
    OSStatus status = noErr;
    const auto data = value(object, address, status);
    release(address, data);
    bytes = static_cast<UInt32>(data.size());
    return status;
  }
  OSStatus get(AudioObjectID object, const AudioObjectPropertyAddress &address, UInt32 &bytes,
               void *out) override {
    OSStatus status = noErr;
    const auto data = value(object, address, status);
    if (status != noErr) {
      return status;
    }
    if (bytes < data.size()) {
      release(address, data);
      return kAudioHardwareBadPropertySizeError;
    }
    std::memcpy(out, data.data(), data.size());
    bytes = static_cast<UInt32>(data.size());
    return noErr;
  }
  OSStatus set(AudioObjectID object, const AudioObjectPropertyAddress &address, UInt32 bytes,
               const void *data) override {
    Device *device = find(object);
    AE_CHECK(device != nullptr);
    const int direction = address.mScope == kAudioObjectPropertyScopeInput ? 0 : 1;
    switch (address.mSelector) {
    case kAudioDevicePropertyNominalSampleRate:
      AE_CHECK(bytes == sizeof(Float64));
      ++rate_sets;
      if (fail_set_rate) {
        return kAudioHardwareUnsupportedOperationError;
      }
      std::memcpy(&device->rate, data, sizeof(Float64));
      emit(object, kAudioDevicePropertyNominalSampleRate);
      emit(object, kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput);
      return noErr;
    case kAudioDevicePropertyBufferFrameSize:
      AE_CHECK(bytes == sizeof(UInt32));
      ++frame_sets;
      if (fail_set_frames) {
        return kAudioHardwareUnsupportedOperationError;
      }
      if (pin_frames == 0) {
        std::memcpy(&device->frames, data, sizeof(UInt32));
      } else {
        device->frames = pin_frames;
      }
      emit(object, kAudioDevicePropertyBufferFrameSize);
      return noErr;
    case kAudioDevicePropertyIOProcStreamUsage: {
      const auto *request = static_cast<const AudioHardwareIOProcStreamUsage *>(data);
      AE_CHECK(request->mIOProc == reinterpret_cast<void *>(proc));
      if (request->mNumberStreams != device->streams[direction].size()) {
        return kAudioHardwareBadPropertySizeError;
      }
      AE_CHECK(bytes == offsetof(AudioHardwareIOProcStreamUsage, mStreamIsOn) +
                            request->mNumberStreams * sizeof(UInt32));
      usage[direction].assign(request->mStreamIsOn, request->mStreamIsOn + request->mNumberStreams);
      if (fail_usage) {
        return kAudioHardwareUnsupportedOperationError;
      }
      return noErr;
    }
    default:
      return kAudioHardwareUnknownPropertyError;
    }
  }
  OSStatus listen(AudioObjectID object, const AudioObjectPropertyAddress &address,
                  AudioObjectPropertyListenerProc listener, void *client) override {
    AE_CHECK(address.mSelector == kAudioObjectPropertySelectorWildcard && client == nullptr);
    if (fail_listen) {
      return kAudioHardwareUnspecifiedError;
    }
    listeners[object] = listener;
    return noErr;
  }
  OSStatus unlisten(AudioObjectID object, const AudioObjectPropertyAddress &,
                    AudioObjectPropertyListenerProc listener, void *) override {
    AE_CHECK(listeners.count(object) != 0 && listeners[object] == listener);
    listeners.erase(object);
    return noErr;
  }
  OSStatus create_io(AudioObjectID device, AudioDeviceIOProc callback, void *client,
                     AudioDeviceIOProcID &id) override {
    AE_CHECK(proc == nullptr && client == nullptr);
    if (fail_create) {
      return kAudioHardwareIllegalOperationError;
    }
    proc = callback;
    owner = device;
    id = callback;
    usage[0].clear();
    usage[1].clear();
    ++creations;
    if (rate_during_create != 0) {
      find(device)->rate = rate_during_create;
      if (notify_during_create) {
        emit(device, kAudioDevicePropertyNominalSampleRate);
      }
    }
    if (frames_during_create != 0) {
      find(device)->frames = frames_during_create;
    }
    if (layout_during_create) {
      find(device)->streams[1].push_back({99, 1, 0});
    }
    return noErr;
  }
  OSStatus destroy_io(AudioObjectID device, AudioDeviceIOProcID id) override {
    ++destructions;
    if (destroy_status != noErr) {
      if (destroy_status == kAudioHardwareBadObjectError) {
        proc = nullptr;
      }
      return destroy_status;
    }
    AE_CHECK(device == owner && id == proc && !running);
    proc = nullptr;
    return noErr;
  }
  OSStatus start(AudioObjectID device, AudioDeviceIOProcID id) override {
    AE_CHECK(device == owner && id == proc);
    ++starts;
    if (fail_start) {
      return kAudioHardwareIllegalOperationError;
    }
    running = true;
    return noErr;
  }
  OSStatus stop(AudioObjectID device, AudioDeviceIOProcID id) override {
    AE_CHECK(device == owner && id == proc);
    stop_entered.store(true);
    stop_entered.notify_all();
    if (stop_status != noErr) {
      if (stop_status == kAudioHardwareBadObjectError) {
        running = false;
      }
      return stop_status;
    }
    if (!running) {
      return kAudioHardwareNotRunningError;
    }
    running = false;
    return noErr;
  }
  OSStatus create_aggregate(CFDictionaryRef description, AudioObjectID &device) override {
    if (fail_aggregate) {
      return kAudioHardwareIllegalOperationError;
    }
    const auto number = [](CFDictionaryRef dictionary, CFStringRef key) {
      int result = -1;
      const auto *stored = static_cast<CFNumberRef>(CFDictionaryGetValue(dictionary, key));
      AE_CHECK(stored != nullptr && CFNumberGetValue(stored, kCFNumberIntType, &result));
      return result;
    };
    const auto string = [](CFDictionaryRef dictionary, CFStringRef key) {
      const auto *stored = static_cast<CFStringRef>(CFDictionaryGetValue(dictionary, key));
      AE_CHECK(stored != nullptr);
      return utf8(stored);
    };
    AE_CHECK(number(description, CFSTR(kAudioAggregateDeviceIsPrivateKey)) == 1);
    AE_CHECK(number(description, CFSTR(kAudioAggregateDeviceIsStackedKey)) == 0);
    aggregate_uid = string(description, CFSTR(kAudioAggregateDeviceUIDKey));
    aggregate_clock = string(description, CFSTR(kAudioAggregateDeviceMainSubDeviceKey));
    const auto *members = static_cast<CFArrayRef>(
        CFDictionaryGetValue(description, CFSTR(kAudioAggregateDeviceSubDeviceListKey)));
    AE_CHECK(members != nullptr && CFArrayGetCount(members) == 2);
    Device combined;
    combined.id = kAggregate;
    combined.uid = aggregate_uid;
    combined.name = "aggregate";
    aggregate_members.clear();
    aggregate_drift.clear();
    for (CFIndex i = 0; i < 2; ++i) {
      const auto *member = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(members, i));
      const std::string identity = string(member, CFSTR(kAudioSubDeviceUIDKey));
      aggregate_members.push_back(identity);
      aggregate_drift.push_back(number(member, CFSTR(kAudioSubDeviceDriftCompensationKey)));
      for (const Device &candidate : devices) {
        if (candidate.uid == identity) {
          for (int direction = 0; direction < 2; ++direction) {
            combined.streams[direction].insert(combined.streams[direction].end(),
                                               candidate.streams[direction].begin(),
                                               candidate.streams[direction].end());
          }
        }
      }
    }
    if (truncate_aggregate) {
      combined.streams[1].pop_back();
    }
    devices.push_back(combined);
    device = kAggregate;
    ++aggregates_created;
    return noErr;
  }
  OSStatus destroy_aggregate(AudioObjectID device) override {
    AE_CHECK(device == kAggregate && proc == nullptr && listeners.empty());
    if (fail_destroy_aggregate) {
      return kAudioHardwareIllegalOperationError;
    }
    AE_CHECK(devices.back().id == kAggregate);
    devices.pop_back();
    ++aggregates_destroyed;
    return noErr;
  }
  ae::CaptureAccess capture_access(bool request) override {
    AE_CHECK(request);
    ++access_queries;
    return access;
  }

  void emit(AudioObjectID object, AudioObjectPropertySelector selector,
            AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    if (listeners.count(object) == 0) {
      return;
    }
    const AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
    AE_CHECK(listeners[object](object, 1, &address, nullptr) == noErr);
  }

  Buffers buffers(int direction, uint32_t frames) {
    const auto &streams = find(owner)->streams[direction];
    Buffers result;
    result.storage.resize(
        (offsetof(AudioBufferList, mBuffers) + streams.size() * sizeof(AudioBuffer) + 7) / 8 + 1);
    result.data.resize(streams.size());
    result.list()->mNumberBuffers = static_cast<UInt32>(streams.size());
    uint32_t first = 0;
    for (size_t i = 0; i < streams.size(); ++i) {
      const uint32_t channels = wrong_channels ? streams[i].channels + 1 : streams[i].channels;
      const uint32_t count = direction == 1 && short_output != 0 ? short_output : frames;
      auto &data = result.data[i];
      data.assign(static_cast<size_t>(count) * channels + 2, kGarbage);
      if (direction == 0) {
        for (uint32_t frame = 0; frame < count; ++frame) {
          for (uint32_t channel = 0; channel < streams[i].channels; ++channel) {
            data[1 + frame * channels + channel] =
                poison ? std::numeric_limits<float>::quiet_NaN() : sample(first + channel, frame);
          }
        }
      }
      AudioBuffer &buffer = result.list()->mBuffers[i];
      buffer.mNumberChannels = channels;
      buffer.mDataByteSize = count * channels * static_cast<UInt32>(sizeof(float));
      const bool enabled = i >= usage[direction].size() || usage[direction][i] != 0;
      buffer.mData = enabled ? data.data() + 1 : nullptr;
      first += streams[i].channels;
    }
    return result;
  }

  void cycle(uint32_t frames) {
    AE_CHECK(proc != nullptr);
    input = buffers(0, frames);
    output = buffers(1, frames);
    forbid_allocation = true;
    AE_CHECK(proc(owner, &time, input.list(), &time, output.list(), &time, nullptr) == noErr);
    forbid_allocation = false;
    for (const auto &data : output.data) {
      AE_CHECK(data.front() == kGarbage && data.back() == kGarbage);
    }
  }

  float out(size_t stream, uint32_t channel, uint32_t frame) {
    const uint32_t channels = output.list()->mBuffers[stream].mNumberChannels;
    return output.data[stream][1 + frame * channels + channel];
  }
  bool silent() {
    for (const auto &data : output.data) {
      for (size_t i = 1; i + 1 < data.size(); ++i) {
        if (data[i] != 0.0f) {
          return false;
        }
      }
    }
    return true;
  }

  std::vector<Device> devices;
  std::map<AudioObjectID, AudioObjectPropertyListenerProc> listeners;
  AudioDeviceIOProc proc = nullptr;
  AudioObjectID owner = 0;
  std::vector<UInt32> usage[2];
  Buffers input, output;
  AudioTimeStamp time{};
  bool running = false;
  bool fail_devices = false, fail_set_rate = false, fail_set_frames = false, fail_usage = false;
  bool fail_listen = false, fail_create = false, fail_start = false, fail_latency = false;
  bool fail_aggregate = false, fail_destroy_aggregate = false, truncate_aggregate = false;
  bool drop_stream = false, notify_during_create = false, layout_during_create = false;
  bool wrong_channels = false, poison = false;
  Float64 rate_during_create = 0;
  UInt32 frames_during_create = 0, pin_frames = 0, short_output = 0;
  OSStatus stop_status = noErr, destroy_status = noErr;
  uint32_t creations = 0, destructions = 0, starts = 0, rate_sets = 0, frame_sets = 0;
  uint32_t aggregates_created = 0, aggregates_destroyed = 0, access_queries = 0;
  std::string aggregate_uid, aggregate_clock;
  std::vector<std::string> aggregate_members;
  std::vector<int> aggregate_drift;
  ae::CaptureAccess access = ae::CaptureAccess::Granted;
  std::atomic<bool> stop_entered{false};

private:
  static void release(const AudioObjectPropertyAddress &address, const std::vector<uint8_t> &data) {
    if (data.size() == sizeof(CFStringRef) &&
        (address.mSelector == kAudioDevicePropertyDeviceUID ||
         address.mSelector == kAudioObjectPropertyName ||
         address.mSelector == kAudioObjectPropertyElementName)) {
      CFStringRef text = nullptr;
      std::memcpy(&text, data.data(), sizeof(text));
      CFRelease(text);
    }
  }
} hal;

ae_context *context() {
  ae_context *result = nullptr;
  auto backend = ae::make_coreaudio_backend(&hal);
  AE_CHECK(backend->refresh() == AE_OK);
  AE_CHECK(ae::detail::create_context_with_backend(std::move(backend), &result) == AE_OK);
  return result;
}

ae_stream_request request() {
  static const uint32_t input[] = {2, 0};
  static const uint32_t output[] = {2, 0};
  ae_stream_request result{};
  result.struct_size = sizeof(result);
  result.backend_id = "coreaudio";
  result.input_device_id = duplex;
  result.output_device_id = duplex;
  result.input_channels = {AE_CHANNEL_MODE_EXPLICIT, 2, input};
  result.output_channels = {AE_CHANNEL_MODE_EXPLICIT, 2, output};
  result.sample_rate = 48000;
  result.frames_per_block = 128;
  return result;
}

ae_stream_status status_of(ae_stream *stream) {
  ae_stream_status status{};
  status.struct_size = sizeof(status);
  AE_CHECK(ae_stream_get_status(stream, &status) == AE_OK);
  return status;
}

ae_diagnostic diagnostic_of(ae_context *ctx) {
  ae_diagnostic diagnostic{};
  diagnostic.struct_size = sizeof(diagnostic);
  AE_CHECK(ae_context_get_diagnostic(ctx, &diagnostic) == AE_OK);
  return diagnostic;
}

template <typename Predicate> void wait_until(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!predicate()) {
    AE_CHECK(std::chrono::steady_clock::now() < deadline);
    std::this_thread::yield();
  }
}

struct Processor {
  std::atomic<uint32_t> calls{0};
  std::atomic<bool> entered{false}, release{true};
  uint32_t frames = 128;
  uint64_t position = 0;
  ae_process_result result = AE_PROCESS_OK;
  bool expect_silence = false;
};

ae_process_result process(void *user, const ae_audio_block *block) {
  auto &processor = *static_cast<Processor *>(user);
  ++processor.calls;
  processor.entered.store(true);
  processor.entered.notify_all();
  while (!processor.release.load()) {
    processor.release.wait(false);
  }
  AE_CHECK(block->input_channels == 2 && block->output_channels == 2);
  AE_CHECK(block->frames == processor.frames && block->frame_position == processor.position);
  processor.position += block->frames;
  for (uint32_t i = 0; i < block->frames; ++i) {
    AE_CHECK(block->inputs[0][i] == (processor.expect_silence ? 0.0f : sample(2, i)));
    AE_CHECK(block->inputs[1][i] == (processor.expect_silence ? 0.0f : sample(0, i)));
    AE_CHECK(block->outputs[0][i] == 0.0f && block->outputs[1][i] == 0.0f);
    block->outputs[0][i] = block->inputs[0][i] * 0.5f;
    block->outputs[1][i] = -0.25f;
  }
  return processor.result;
}

void check_routed(uint32_t frames) {
  for (uint32_t frame = 0; frame < frames; ++frame) {
    AE_CHECK(hal.out(0, 0, frame) == -0.25f);
    AE_CHECK(hal.out(1, 0, frame) == 0.0f);
    AE_CHECK(hal.out(1, 1, frame) == sample(2, frame) * 0.5f);
  }
}

void enumeration() {
  auto *ctx = context();
  ae_device_snapshot *devices = nullptr;
  AE_CHECK(ae_context_enumerate_devices(ctx, &devices) == AE_OK);
  AE_CHECK(ae_device_snapshot_count(devices) == 3);
  const char *ids[] = {duplex, playback, mixed};
  const uint32_t inputs[] = {3, 0, 1}, outputs[] = {3, 2, 2};
  for (uint32_t i = 0; i < 3; ++i) {
    ae_device_info info{};
    info.struct_size = sizeof(info);
    AE_CHECK(ae_device_snapshot_get(devices, i, &info) == AE_OK);
    AE_CHECK(std::strcmp(info.backend_id, "coreaudio") == 0);
    AE_CHECK(std::strcmp(info.device_id, ids[i]) == 0);
    AE_CHECK(info.input_channels == inputs[i] && info.output_channels == outputs[i]);
    AE_CHECK(info.channel_counts_known == 1 && info.flags == AE_DEVICE_PAIRABLE);
  }
  AE_CHECK(ae_device_snapshot_release(devices) == AE_OK);
  hal.devices.erase(hal.devices.begin() + 1);
  AE_CHECK(ae_context_refresh_devices(ctx) == AE_OK);
  AE_CHECK(ae_context_enumerate_devices(ctx, &devices) == AE_OK);
  AE_CHECK(ae_device_snapshot_count(devices) == 2);
  AE_CHECK(ae_device_snapshot_release(devices) == AE_OK);
  hal.fail_devices = true;
  AE_CHECK(ae_context_refresh_devices(ctx) == AE_ERROR_DRIVER_FAILURE);
  AE_CHECK(diagnostic_of(ctx).native_code == kAudioHardwareUnspecifiedError);
  hal.fail_devices = false;
  AE_CHECK(ae_context_enumerate_devices(ctx, &devices) == AE_OK);
  AE_CHECK(ae_device_snapshot_count(devices) == 2);
  AE_CHECK(ae_device_snapshot_release(devices) == AE_OK);
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
  hal.reset();
}

void lifecycle() {
  auto *ctx = context();
  ae_capability_snapshot *snapshot = nullptr;
  AE_CHECK(ae_context_probe_device(ctx, "coreaudio", duplex, &snapshot) == AE_OK);
  AE_CHECK(hal.listeners.empty() && hal.creations == 0);
  ae_device_capabilities caps{};
  caps.struct_size = sizeof(caps);
  AE_CHECK(ae_capability_snapshot_get(snapshot, &caps) == AE_OK);
  AE_CHECK(caps.input_channels == 3 && caps.output_channels == 3);
  AE_CHECK(caps.current_sample_rate == 48000 && caps.tested_rate_count == 3);
  AE_CHECK(caps.tested_rates[0] == 44100 && caps.tested_rates[2] == 96000);
  AE_CHECK(caps.min_frames == 16 && caps.max_frames == 1024 && caps.preferred_frames == 512);
  AE_CHECK(caps.frame_granularity == 1);
  ae_channel_info channel{};
  channel.struct_size = sizeof(channel);
  AE_CHECK(ae_capability_snapshot_get_channel(snapshot, AE_DIRECTION_INPUT, 0, &channel) == AE_OK);
  AE_CHECK(channel.supported == 1 && channel.native_format == 24 && channel.index == 0);
  AE_CHECK(std::strcmp(channel.name, "Mic") == 0);
  AE_CHECK(ae_capability_snapshot_get_channel(snapshot, AE_DIRECTION_OUTPUT, 2, &channel) == AE_OK);
  AE_CHECK(std::strcmp(channel.name, "Output 3") == 0);
  AE_CHECK(ae_capability_snapshot_release(snapshot) == AE_OK);

  auto spec = request();
  ae_stream_format format{};
  format.struct_size = sizeof(format);
  ae_stream *stream = nullptr;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  AE_CHECK(format.sample_rate == 48000 && format.frames_per_block == 128);
  AE_CHECK(format.max_frames_per_block == 128);
  AE_CHECK(hal.find(kDuplex)->frames == 128 && hal.rate_sets == 0 && hal.frame_sets == 1);
  AE_CHECK(hal.creations == 1 && hal.listeners.count(kDuplex) == 1 && hal.access_queries == 1);
  AE_CHECK(hal.usage[0] == std::vector<UInt32>({1, 1}) &&
           hal.usage[1] == std::vector<UInt32>({1, 1}));
  uint32_t physical = 0;
  AE_CHECK(ae_stream_get_channel(stream, AE_DIRECTION_INPUT, 0, &physical) == AE_OK);
  AE_CHECK(physical == 2);
  AE_CHECK(ae_stream_get_channel(stream, AE_DIRECTION_OUTPUT, 1, &physical) == AE_OK);
  AE_CHECK(physical == 0);
  hal.cycle(128);
  AE_CHECK(hal.silent());

  auto *other = context();
  ae_stream *second = nullptr;
  AE_CHECK(ae_stream_open(other, &spec, &second, &format) == AE_ERROR_BUSY);
  AE_CHECK(second == nullptr);
  AE_CHECK(ae_context_probe_device(other, "coreaudio", duplex, &snapshot) == AE_OK);
  AE_CHECK(ae_capability_snapshot_release(snapshot) == AE_OK);
  AE_CHECK(ae_context_destroy(other) == AE_OK);
  AE_CHECK(hal.listeners.count(kDuplex) == 1);

  Processor processor;
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_OK);
  AE_CHECK(hal.running && processor.calls == 0);
  hal.cycle(128);
  AE_CHECK(processor.calls == 1);
  check_routed(128);
  auto status = status_of(stream);
  AE_CHECK(status.state == AE_STREAM_STATE_RUNNING && status.fault_flags == 0);
  AE_CHECK(status.callback_count == 1 && status.delivered_frames == 128);
  AE_CHECK(status.latency_valid == 1 && status.input_latency_frames == 10 + 20 + 5 + 128);
  AE_CHECK(status.output_latency_frames == 30 + 40 + 7 + 128);
  hal.find(kDuplex)->rate = 96000;
  hal.emit(kDuplex, kAudioDevicePropertyNominalSampleRate);
  status = status_of(stream);
  AE_CHECK(status.fault_flags == AE_FAULT_SAMPLE_RATE);
  AE_CHECK(status.state == AE_STREAM_STATE_RECOVERY_REQUIRED);
  AE_CHECK(status.last_result == AE_ERROR_CONFIGURATION_CHANGED);
  hal.cycle(128);
  AE_CHECK(processor.calls == 1 && hal.silent());
  AE_CHECK(ae_stream_stop(stream) == AE_OK);
  AE_CHECK(!hal.running);
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_ERROR_INVALID_STATE);
  hal.destroy_status = kAudioHardwareIllegalOperationError;
  AE_CHECK(ae_stream_close(stream) == AE_ERROR_DRIVER_FAILURE);
  AE_CHECK(diagnostic_of(ctx).native_code == kAudioHardwareIllegalOperationError);
  AE_CHECK(ae_context_destroy(ctx) == AE_ERROR_BUSY);
  hal.destroy_status = noErr;
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(hal.proc == nullptr && hal.listeners.empty());
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
  hal.reset();
}

void negotiation() {
  auto *ctx = context();
  auto spec = request();
  ae_stream_format format{};
  format.struct_size = sizeof(format);
  ae_stream *stream = nullptr;
  spec.sample_rate = 88200;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_UNSUPPORTED_CONFIGURATION);
  AE_CHECK(hal.listeners.empty() && hal.proc == nullptr);
  spec.negotiation = AE_NEGOTIATION_ALLOW_ADJUSTMENT;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  AE_CHECK(format.sample_rate == 48000 && hal.rate_sets == 0);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  spec.negotiation = AE_NEGOTIATION_EXACT;
  spec.sample_rate = 44100;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  AE_CHECK(format.sample_rate == 44100 && hal.find(kDuplex)->rate == 44100 && hal.rate_sets == 1);
  AE_CHECK(status_of(stream).fault_flags == 0);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  spec.sample_rate = 0;
  spec.frames_per_block = 0;
  hal.find(kDuplex)->frames = 256;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  AE_CHECK(format.sample_rate == 44100 && format.frames_per_block == 256);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  spec.sample_rate = 48000;
  hal.fail_set_rate = true;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_DRIVER_FAILURE);
  AE_CHECK(diagnostic_of(ctx).native_code == kAudioHardwareUnsupportedOperationError);
  hal.fail_set_rate = false;
  for (uint32_t frames : {8u, 2048u}) {
    spec.frames_per_block = frames;
    AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_UNSUPPORTED_CONFIGURATION);
    spec.negotiation = AE_NEGOTIATION_ALLOW_ADJUSTMENT;
    AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
    AE_CHECK(format.frames_per_block == 256);
    AE_CHECK(ae_stream_close(stream) == AE_OK);
    spec.negotiation = AE_NEGOTIATION_EXACT;
  }
  spec.frames_per_block = 100;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  AE_CHECK(format.frames_per_block == 100);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  spec.frames_per_block = 64;
  hal.fail_set_frames = true;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_DRIVER_FAILURE);
  hal.fail_set_frames = false;
  hal.find(kDuplex)->variable = 96;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  AE_CHECK(format.frames_per_block == 64 && format.max_frames_per_block == 96);
  Processor processor;
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_OK);
  for (uint32_t frames : {64u, 96u, 17u}) {
    processor.frames = frames;
    hal.cycle(frames);
    check_routed(frames);
  }
  AE_CHECK(processor.calls == 3 && status_of(stream).delivered_frames == 177);
  hal.cycle(97);
  AE_CHECK(processor.calls == 3 && hal.silent());
  AE_CHECK(status_of(stream).fault_flags == AE_FAULT_CALLBACK);
  AE_CHECK(status_of(stream).last_result == AE_ERROR_DRIVER_FAILURE);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
  hal.reset();
}

void pinned_buffer() {
  auto *ctx = context();
  auto spec = request();
  ae_stream_format format{};
  format.struct_size = sizeof(format);
  ae_stream *stream = nullptr;
  hal.pin_frames = 512;
  const auto begin = std::chrono::steady_clock::now();
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_UNSUPPORTED_CONFIGURATION);
  AE_CHECK(std::chrono::steady_clock::now() - begin >= std::chrono::seconds(2));
  AE_CHECK(diagnostic_of(ctx).native_code == 512);
  hal.pin_frames = 0;
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
  hal.reset();
}

void failures() {
  auto *ctx = context();
  auto spec = request();
  ae_stream_format format{};
  format.struct_size = sizeof(format);
  ae_stream *stream = nullptr;
  hal.fail_listen = true;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_DRIVER_FAILURE);
  hal.fail_listen = false;
  hal.drop_stream = true;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_DRIVER_FAILURE);
  hal.drop_stream = false;
  hal.find(kDuplex)->streams[0][1].floating = false;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_UNSUPPORTED_CONFIGURATION);
  AE_CHECK(diagnostic_of(ctx).native_code == 24);
  ae_capability_snapshot *snapshot = nullptr;
  AE_CHECK(ae_context_probe_device(ctx, "coreaudio", duplex, &snapshot) == AE_OK);
  ae_channel_info channel{};
  channel.struct_size = sizeof(channel);
  AE_CHECK(ae_capability_snapshot_get_channel(snapshot, AE_DIRECTION_INPUT, 2, &channel) == AE_OK);
  AE_CHECK(channel.supported == 0);
  AE_CHECK(ae_capability_snapshot_get_channel(snapshot, AE_DIRECTION_INPUT, 1, &channel) == AE_OK);
  AE_CHECK(channel.supported == 1);
  AE_CHECK(ae_capability_snapshot_release(snapshot) == AE_OK);
  hal.find(kDuplex)->streams[0][1].floating = true;
  hal.fail_create = true;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_DRIVER_FAILURE);
  AE_CHECK(stream == nullptr && hal.listeners.empty());
  hal.fail_create = false;
  hal.devices.erase(hal.devices.begin());
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_DEVICE_UNAVAILABLE);
  AE_CHECK(ae_context_probe_device(ctx, "coreaudio", duplex, &snapshot) ==
           AE_ERROR_DEVICE_UNAVAILABLE);
  hal.reset();
  hal.fail_usage = true;
  hal.fail_latency = true;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  AE_CHECK(status_of(stream).latency_valid == 0);
  hal.fail_usage = false;
  hal.fail_latency = false;
  hal.emit(kDuplex, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput);
  AE_CHECK(status_of(stream).latency_valid == 1);
  Processor processor;
  hal.fail_start = true;
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_ERROR_DRIVER_FAILURE);
  AE_CHECK(diagnostic_of(ctx).native_code == kAudioHardwareIllegalOperationError);
  auto status = status_of(stream);
  AE_CHECK(status.state == AE_STREAM_STATE_READY && status.run_generation == 1);
  hal.stop_status = kAudioHardwareIllegalOperationError;
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_ERROR_QUIESCENCE_FAILURE);
  AE_CHECK(ae_stream_close(stream) == AE_ERROR_QUIESCENCE_FAILURE);
  hal.stop_status = noErr;
  hal.fail_start = false;
  AE_CHECK(ae_stream_stop(stream) == AE_OK);
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_OK);
  hal.cycle(128);
  AE_CHECK(processor.calls == 1 && status_of(stream).fault_flags == 0);
  AE_CHECK(ae_stream_stop(stream) == AE_OK);
  processor.position = 0;
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_OK);
  hal.cycle(128);
  AE_CHECK(processor.calls == 2 && status_of(stream).callback_count == 1);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
  hal.reset();
}

void permission() {
  auto *ctx = context();
  auto spec = request();
  ae_stream_format format{};
  format.struct_size = sizeof(format);
  ae_stream *stream = nullptr;
  hal.access = ae::CaptureAccess::Denied;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_PERMISSION_DENIED);
  AE_CHECK(stream == nullptr && hal.creations == 0 && hal.listeners.empty());
  AE_CHECK(diagnostic_of(ctx).result == AE_ERROR_PERMISSION_DENIED);
  const uint32_t queries = hal.access_queries;
  spec.input_channels = {AE_CHANNEL_MODE_NONE, 0, nullptr};
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  AE_CHECK(hal.access_queries == queries && format.input_channels == 0);
  AE_CHECK(hal.usage[0] == std::vector<UInt32>({0, 0}));
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  hal.access = ae::CaptureAccess::Undetermined;
  spec = request();
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
  hal.reset();
  hal.access = ae::CaptureAccess::Granted;
}

void preparation_changes() {
  auto *ctx = context();
  auto spec = request();
  ae_stream_format format{};
  format.struct_size = sizeof(format);
  ae_stream *stream = nullptr;
  for (bool notified : {false, true}) {
    hal.rate_during_create = 96000;
    hal.notify_during_create = notified;
    AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_CONFIGURATION_CHANGED);
    AE_CHECK(stream == nullptr && hal.proc == nullptr && hal.listeners.empty());
    hal.rate_during_create = 0;
    hal.find(kDuplex)->rate = 48000;
  }
  hal.notify_during_create = false;
  hal.frames_during_create = 64;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_CONFIGURATION_CHANGED);
  hal.frames_during_create = 0;
  hal.layout_during_create = true;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_CONFIGURATION_CHANGED);
  AE_CHECK(diagnostic_of(ctx).result == AE_ERROR_CONFIGURATION_CHANGED);
  hal.layout_during_create = false;
  hal.reset();
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  hal.find(kDuplex)->frames = 64;
  hal.emit(kDuplex, kAudioDevicePropertyBufferFrameSize);
  Processor processor;
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_ERROR_INVALID_STATE);
  AE_CHECK(processor.calls == 0);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
  hal.reset();
}

void notifications() {
  auto *ctx = context();
  auto spec = request();
  ae_stream_format format{};
  format.struct_size = sizeof(format);
  ae_stream *stream = nullptr;
  Processor processor;
  const auto reopen = [&] {
    if (stream != nullptr) {
      AE_CHECK(ae_stream_close(stream) == AE_OK);
    }
    hal.reset();
    processor.position = 0;
    AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
    AE_CHECK(ae_stream_start(stream, process, &processor) == AE_OK);
  };
  reopen();
  for (AudioObjectPropertySelector selector : std::initializer_list<AudioObjectPropertySelector>{
           kAudioDevicePropertyNominalSampleRate, kAudioDevicePropertyBufferFrameSize,
           kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyStreams,
           kAudioDevicePropertyDeviceIsAlive, kAudioDevicePropertyVolumeScalar,
           kAudioDevicePropertyDeviceIsRunning}) {
    hal.emit(kDuplex, selector);
    hal.emit(kPlayback, selector);
  }
  AE_CHECK(status_of(stream).fault_flags == 0);
  AE_CHECK(status_of(stream).state == AE_STREAM_STATE_RUNNING);
  for (uint32_t count = 1; count <= 3; ++count) {
    hal.emit(kDuplex, kAudioDeviceProcessorOverload);
    AE_CHECK(status_of(stream).driver_overloads == count && status_of(stream).fault_flags == 0);
  }
  hal.find(kDuplex)->latency[0] = 100;
  hal.find(kDuplex)->safety[1] = 200;
  hal.emit(kDuplex, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput);
  auto status = status_of(stream);
  AE_CHECK(status.latency_valid == 1 && status.input_latency_frames == 100 + 20 + 5 + 128);
  AE_CHECK(status.output_latency_frames == 30 + 200 + 7 + 128 && status.fault_flags == 0);
  hal.fail_latency = true;
  hal.emit(kDuplex, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput);
  AE_CHECK(status_of(stream).latency_valid == 0 && status_of(stream).fault_flags == 0);
  hal.fail_latency = false;

  hal.find(kDuplex)->rate = 0;
  hal.emit(kDuplex, kAudioDevicePropertyNominalSampleRate);
  AE_CHECK(status_of(stream).fault_flags == (AE_FAULT_SAMPLE_RATE | AE_FAULT_CLOCK_LOST));
  reopen();
  hal.find(kDuplex)->frames = 256;
  hal.emit(kDuplex, kAudioDevicePropertyBufferFrameSize);
  AE_CHECK(status_of(stream).fault_flags == AE_FAULT_RESET);
  AE_CHECK(status_of(stream).last_result == AE_ERROR_CONFIGURATION_CHANGED);
  reopen();
  hal.find(kDuplex)->streams[0].pop_back();
  hal.emit(kDuplex, kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput);
  AE_CHECK(status_of(stream).fault_flags == AE_FAULT_RESET);
  reopen();
  hal.find(kDuplex)->streams[1][0].channels = 2;
  hal.emit(kDuplex, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
  AE_CHECK(status_of(stream).fault_flags == AE_FAULT_RESET);
  reopen();
  hal.find(kDuplex)->alive = 0;
  hal.emit(kDuplex, kAudioDevicePropertyDeviceIsAlive);
  status = status_of(stream);
  AE_CHECK(status.fault_flags == AE_FAULT_DEVICE);
  AE_CHECK(status.state == AE_STREAM_STATE_RECOVERY_REQUIRED);
  AE_CHECK(status.last_result == AE_ERROR_DEVICE_UNAVAILABLE);
  hal.cycle(128);
  AE_CHECK(processor.calls == 0 && hal.silent());
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
  hal.reset();
}

void removal() {
  auto *ctx = context();
  auto spec = request();
  ae_stream_format format{};
  format.struct_size = sizeof(format);
  ae_stream *stream = nullptr;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  Processor processor;
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_OK);
  hal.cycle(128);
  hal.devices.erase(hal.devices.begin());
  hal.listeners[kDuplex](kDuplex, 1,
                         &(const AudioObjectPropertyAddress &)AudioObjectPropertyAddress{
                             kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
                             kAudioObjectPropertyElementMain},
                         nullptr);
  AE_CHECK(status_of(stream).fault_flags == AE_FAULT_DEVICE);
  hal.stop_status = kAudioHardwareBadObjectError;
  hal.destroy_status = kAudioHardwareBadObjectError;
  AE_CHECK(ae_stream_stop(stream) == AE_OK);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  hal.stop_status = noErr;
  hal.destroy_status = noErr;
  hal.reset();
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  AE_CHECK(status_of(stream).fault_flags == 0);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
  hal.reset();
}

void callback_validation() {
  auto *ctx = context();
  auto spec = request();
  ae_stream_format format{};
  format.struct_size = sizeof(format);
  ae_stream *stream = nullptr;
  Processor processor;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_OK);
  hal.usage[0] = {0, 0};
  processor.expect_silence = true;
  hal.cycle(128);
  AE_CHECK(processor.calls == 1 && status_of(stream).fault_flags == 0);
  for (uint32_t frame = 0; frame < 128; ++frame) {
    AE_CHECK(hal.out(0, 0, frame) == -0.25f && hal.out(1, 1, frame) == 0.0f);
  }
  hal.usage[0] = {1, 1};
  hal.poison = true;
  hal.cycle(128);
  AE_CHECK(processor.calls == 2 && status_of(stream).fault_flags == 0);
  hal.poison = false;
  processor.expect_silence = false;
  processor.result = AE_PROCESS_SILENCE;
  hal.cycle(128);
  AE_CHECK(processor.calls == 3 && hal.silent() && status_of(stream).fault_flags == 0);
  processor.result = AE_PROCESS_OK;
  hal.usage[0] = {0, 0};
  hal.usage[1] = {0, 0};
  hal.cycle(128);
  AE_CHECK(processor.calls == 3 && status_of(stream).fault_flags == 0);
  AE_CHECK(status_of(stream).callback_count == 4);
  hal.usage[0] = {1, 1};
  hal.usage[1] = {1, 1};
  hal.short_output = 64;
  hal.cycle(128);
  AE_CHECK(processor.calls == 3 && hal.silent());
  AE_CHECK(status_of(stream).fault_flags == AE_FAULT_CALLBACK);
  hal.short_output = 0;
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  processor.position = 0;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_OK);
  hal.wrong_channels = true;
  hal.cycle(128);
  AE_CHECK(processor.calls == 3 && hal.silent());
  AE_CHECK(status_of(stream).fault_flags == AE_FAULT_CALLBACK);
  hal.wrong_channels = false;
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_OK);
  processor.result = AE_PROCESS_FAILED;
  processor.position = 0;
  hal.cycle(128);
  AE_CHECK(processor.calls == 4 && hal.silent());
  auto status = status_of(stream);
  AE_CHECK(status.state == AE_STREAM_STATE_RECOVERY_REQUIRED && status.fault_flags == 0);
  AE_CHECK(status.last_result == AE_ERROR_PROCESSOR_FAILURE);
  hal.cycle(128);
  AE_CHECK(processor.calls == 4 && hal.silent());
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  const auto retired = hal.proc;
  AE_CHECK(retired == nullptr);
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
  hal.reset();
}

void in_flight_stop() {
  auto *ctx = context();
  auto spec = request();
  ae_stream_format format{};
  format.struct_size = sizeof(format);
  ae_stream *stream = nullptr;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  Processor processor;
  processor.release.store(false);
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_OK);
  std::thread audio([] { hal.cycle(128); });
  while (!processor.entered.load()) {
    processor.entered.wait(false);
  }
  const auto proc = hal.proc;
  Buffers extra_input = hal.buffers(0, 128), extra_output = hal.buffers(1, 128);
  AE_CHECK(proc(kDuplex, &hal.time, extra_input.list(), &hal.time, extra_output.list(), &hal.time,
                nullptr) == noErr);
  AE_CHECK(processor.calls == 1 && status_of(stream).fault_flags == AE_FAULT_CALLBACK);
  for (const auto &data : extra_output.data) {
    for (size_t i = 1; i + 1 < data.size(); ++i) {
      AE_CHECK(data[i] == 0.0f);
    }
  }
  hal.stop_entered.store(false);
  std::atomic<bool> stopped{false};
  std::thread stopper([&] {
    AE_CHECK(ae_stream_stop(stream) == AE_OK);
    stopped.store(true);
  });
  while (!hal.stop_entered.load()) {
    hal.stop_entered.wait(false);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  AE_CHECK(!stopped.load());
  processor.release.store(true);
  processor.release.notify_all();
  stopper.join();
  audio.join();
  AE_CHECK(stopped.load() && hal.silent());
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_ERROR_INVALID_STATE);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
  proc(kDuplex, &hal.time, extra_input.list(), &hal.time, extra_output.list(), &hal.time, nullptr);
  hal.emit(kDuplex, kAudioDevicePropertyDeviceIsAlive);
  hal.reset();
}

void stall() {
  auto *ctx = context();
  auto spec = request();
  ae_stream_format format{};
  format.struct_size = sizeof(format);
  ae_stream *stream = nullptr;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));
  AE_CHECK(status_of(stream).fault_flags == 0);
  Processor processor;
  AE_CHECK(ae_stream_start(stream, process, &processor) == AE_OK);
  hal.cycle(128);
  AE_CHECK(status_of(stream).fault_flags == 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));
  const auto status = status_of(stream);
  AE_CHECK(status.fault_flags == AE_FAULT_DEVICE);
  AE_CHECK(status.last_result == AE_ERROR_DEVICE_UNAVAILABLE);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
  hal.reset();

  const uint64_t frequency = 1000000000;
  const uint64_t timeout = ae::detail::stall_timeout(frequency, 128, 48000);
  AE_CHECK(timeout == frequency * 2);
  std::atomic<uint64_t> last{frequency};
  AE_CHECK(!ae::detail::callback_stalled(last, timeout, [&] { return frequency - 1; }));
  AE_CHECK(!ae::detail::callback_stalled(last, timeout, [&] { return frequency + timeout; }));
  AE_CHECK(ae::detail::callback_stalled(last, timeout, [&] { return frequency + timeout + 1; }));
  AE_CHECK(!ae::detail::callback_stalled(last, timeout, [&] {
    last.store(frequency + 2);
    return frequency + 1;
  }));
  AE_CHECK(ae::detail::stall_timeout(frequency, 65536, 8000) == frequency * 65536 / 8000 * 3);
}

struct SplitProcessor {
  uint32_t calls = 0;
};

ae_process_result split_process(void *user, const ae_audio_block *block) {
  auto &processor = *static_cast<SplitProcessor *>(user);
  ++processor.calls;
  AE_CHECK(block->input_channels == 1 && block->output_channels == 2 && block->frames == 128);
  for (uint32_t i = 0; i < block->frames; ++i) {
    AE_CHECK(block->inputs[0][i] == sample(1, i));
    block->outputs[0][i] = block->inputs[0][i];
    block->outputs[1][i] = -block->inputs[0][i];
  }
  return AE_PROCESS_OK;
}

void split_devices() {
  auto *ctx = context();
  static const uint32_t input[] = {1};
  auto spec = request();
  spec.output_device_id = mixed;
  spec.input_channels = {AE_CHANNEL_MODE_EXPLICIT, 1, input};
  spec.output_channels = {AE_CHANNEL_MODE_BACKEND_DEFAULT, 0, nullptr};
  ae_stream_format format{};
  format.struct_size = sizeof(format);
  ae_stream *stream = nullptr;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  AE_CHECK(hal.aggregates_created == 1 && hal.owner == kAggregate);
  AE_CHECK(hal.aggregate_uid.starts_with("audio-io-core.aggregate."));
  AE_CHECK(hal.aggregate_members == std::vector<std::string>({duplex, mixed}));
  AE_CHECK(hal.aggregate_drift == std::vector<int>({1, 0}) && hal.aggregate_clock == mixed);
  AE_CHECK(format.input_channels == 1 && format.output_channels == 2);
  AE_CHECK(hal.listeners.size() == 3 && hal.find(kAggregate)->frames == 128);
  AE_CHECK(hal.usage[0] == std::vector<UInt32>({1, 0, 0}));
  AE_CHECK(hal.usage[1] == std::vector<UInt32>({0, 0, 1}));
  uint32_t physical = 9;
  AE_CHECK(ae_stream_get_channel(stream, AE_DIRECTION_OUTPUT, 1, &physical) == AE_OK);
  AE_CHECK(physical == 1);
  AE_CHECK(ae_stream_get_channel(stream, AE_DIRECTION_INPUT, 0, &physical) == AE_OK);
  AE_CHECK(physical == 1);
  ae_device_snapshot *devices = nullptr;
  AE_CHECK(ae_context_refresh_devices(ctx) == AE_OK);
  AE_CHECK(ae_context_enumerate_devices(ctx, &devices) == AE_OK);
  AE_CHECK(ae_device_snapshot_count(devices) == 3);
  AE_CHECK(ae_device_snapshot_release(devices) == AE_OK);
  SplitProcessor processor;
  AE_CHECK(ae_stream_start(stream, split_process, &processor) == AE_OK);
  hal.cycle(128);
  AE_CHECK(processor.calls == 1 && hal.output.list()->mBuffers[0].mData == nullptr);
  for (uint32_t frame = 0; frame < 128; ++frame) {
    AE_CHECK(hal.out(2, 0, frame) == sample(1, frame) && hal.out(2, 1, frame) == -sample(1, frame));
  }
  AE_CHECK(status_of(stream).latency_valid == 1 && status_of(stream).fault_flags == 0);
  hal.find(kMixed)->alive = 0;
  hal.emit(kMixed, kAudioDevicePropertyDeviceIsAlive);
  AE_CHECK(status_of(stream).fault_flags == AE_FAULT_DEVICE);
  hal.find(kMixed)->alive = 1;
  hal.fail_destroy_aggregate = true;
  AE_CHECK(ae_stream_close(stream) == AE_ERROR_DRIVER_FAILURE);
  AE_CHECK(ae_context_destroy(ctx) == AE_ERROR_BUSY);
  hal.fail_destroy_aggregate = false;
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  AE_CHECK(hal.aggregates_destroyed == 1 && hal.find(kAggregate) == nullptr);

  hal.fail_aggregate = true;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_DEVICE_UNAVAILABLE);
  hal.fail_aggregate = false;
  hal.truncate_aggregate = true;
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_DEVICE_UNAVAILABLE);
  AE_CHECK(hal.aggregates_created == 2 && hal.aggregates_destroyed == 2 && stream == nullptr);
  hal.truncate_aggregate = false;
  static const uint32_t beyond[] = {2};
  spec.output_channels = {AE_CHANNEL_MODE_EXPLICIT, 1, beyond};
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_INVALID_ARGUMENT);
  AE_CHECK(hal.aggregates_created == 2);

  spec.output_channels = {AE_CHANNEL_MODE_BACKEND_DEFAULT, 0, nullptr};
  spec.input_channels = {AE_CHANNEL_MODE_NONE, 0, nullptr};
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_OK);
  AE_CHECK(hal.aggregates_created == 2 && hal.owner == kMixed && format.output_channels == 2);
  AE_CHECK(ae_stream_close(stream) == AE_OK);
  spec.input_device_id = playback;
  spec.output_device_id = duplex;
  spec.input_channels = {AE_CHANNEL_MODE_BACKEND_DEFAULT, 0, nullptr};
  AE_CHECK(ae_stream_open(ctx, &spec, &stream, &format) == AE_ERROR_UNSUPPORTED_CONFIGURATION);
  AE_CHECK(hal.aggregates_created == 2);
  AE_CHECK(ae_context_destroy(ctx) == AE_OK);
  hal.reset();
}
}

int main() {
  enumeration();
  lifecycle();
  negotiation();
  pinned_buffer();
  failures();
  permission();
  preparation_changes();
  notifications();
  removal();
  callback_validation();
  in_flight_stop();
  stall();
  split_devices();
}
