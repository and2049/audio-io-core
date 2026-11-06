#include "coreaudio_backend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>
#include <objc/message.h>
#include <objc/runtime.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <utility>

#include "callback_timing.h"

namespace ae {
namespace {

constexpr uint64_t kFrequency = 1000000000;
constexpr uint32_t kUnmeasured = UINT32_MAX;
constexpr const char *kAggregatePrefix = "audio-io-core.aggregate.";
constexpr AudioObjectPropertyAddress kAnyProperty{kAudioObjectPropertySelectorWildcard,
                                                  kAudioObjectPropertyScopeWildcard,
                                                  kAudioObjectPropertyElementWildcard};

AudioObjectPropertyAddress
property(AudioObjectPropertySelector selector,
         AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal,
         AudioObjectPropertyElement element = kAudioObjectPropertyElementMain) {
  return {selector, scope, element};
}

AudioObjectPropertyScope scope(bool input) {
  return input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
}

uint64_t now() noexcept { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

bool gone(OSStatus status) {
  return status == kAudioHardwareBadObjectError || status == kAudioHardwareBadDeviceError;
}

class SystemHal final : public CoreAudioHal {
public:
  OSStatus size(AudioObjectID object, const AudioObjectPropertyAddress &address,
                UInt32 &bytes) override {
    return AudioObjectGetPropertyDataSize(object, &address, 0, nullptr, &bytes);
  }
  OSStatus get(AudioObjectID object, const AudioObjectPropertyAddress &address, UInt32 &bytes,
               void *data) override {
    return AudioObjectGetPropertyData(object, &address, 0, nullptr, &bytes, data);
  }
  OSStatus set(AudioObjectID object, const AudioObjectPropertyAddress &address, UInt32 bytes,
               const void *data) override {
    return AudioObjectSetPropertyData(object, &address, 0, nullptr, bytes, data);
  }
  OSStatus listen(AudioObjectID object, const AudioObjectPropertyAddress &address,
                  AudioObjectPropertyListenerProc listener, void *client) override {
    return AudioObjectAddPropertyListener(object, &address, listener, client);
  }
  OSStatus unlisten(AudioObjectID object, const AudioObjectPropertyAddress &address,
                    AudioObjectPropertyListenerProc listener, void *client) override {
    return AudioObjectRemovePropertyListener(object, &address, listener, client);
  }
  OSStatus create_io(AudioObjectID device, AudioDeviceIOProc proc, void *client,
                     AudioDeviceIOProcID &id) override {
    return AudioDeviceCreateIOProcID(device, proc, client, &id);
  }
  OSStatus destroy_io(AudioObjectID device, AudioDeviceIOProcID id) override {
    return AudioDeviceDestroyIOProcID(device, id);
  }
  OSStatus start(AudioObjectID device, AudioDeviceIOProcID id) override {
    return AudioDeviceStart(device, id);
  }
  OSStatus stop(AudioObjectID device, AudioDeviceIOProcID id) override {
    return AudioDeviceStop(device, id);
  }
  OSStatus create_aggregate(CFDictionaryRef description, AudioObjectID &device) override {
    return AudioHardwareCreateAggregateDevice(description, &device);
  }
  OSStatus destroy_aggregate(AudioObjectID device) override {
    return AudioHardwareDestroyAggregateDevice(device);
  }
  CaptureAccess capture_access(bool request) override {
    const Class device = objc_getClass("AVCaptureDevice");
    if (device == nullptr) {
      return CaptureAccess::Undetermined;
    }
    const auto query = [device] {
      return reinterpret_cast<long (*)(Class, SEL, CFStringRef)>(objc_msgSend)(
          device, sel_registerName("authorizationStatusForMediaType:"), CFSTR("soun"));
    };
    long status = query();
    if (status == 0 && request) {
      std::atomic<bool> answered{false};
      std::atomic<bool> *flag = &answered;
      reinterpret_cast<void (*)(Class, SEL, CFStringRef, void (^)(BOOL))>(objc_msgSend)(
          device, sel_registerName("requestAccessForMediaType:completionHandler:"), CFSTR("soun"),
          ^(BOOL) {
            flag->store(true);
          });
      while (!answered.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      status = query();
    }
    return status == 3   ? CaptureAccess::Granted
           : status == 0 ? CaptureAccess::Undetermined
                         : CaptureAccess::Denied;
  }
};

SystemHal system_hal;

template <typename T> class Owned {
public:
  explicit Owned(T value) : value_(value) {
    if (value_ == nullptr) {
      throw std::bad_alloc();
    }
  }
  ~Owned() { CFRelease(value_); }
  Owned(const Owned &) = delete;
  Owned &operator=(const Owned &) = delete;
  T get() const { return value_; }

private:
  T value_;
};

std::string utf8(CFStringRef value) {
  const CFIndex capacity =
      CFStringGetMaximumSizeForEncoding(CFStringGetLength(value), kCFStringEncodingUTF8) + 1;
  std::string result(static_cast<size_t>(capacity), '\0');
  if (!CFStringGetCString(value, result.data(), capacity, kCFStringEncodingUTF8)) {
    return {};
  }
  result.resize(std::strlen(result.c_str()));
  return result;
}

template <size_t Size> void copy_name(const std::string &name, char (&out)[Size]) {
  size_t length = std::min(name.size(), Size - 1);
  while (length > 0 && length < name.size() &&
         (static_cast<unsigned char>(name[length]) & 0xC0) == 0x80) {
    --length;
  }
  std::memcpy(out, name.data(), length);
  out[length] = '\0';
}

bool float_format(const AudioStreamBasicDescription &format) {
  return format.mFormatID == kAudioFormatLinearPCM &&
         (format.mFormatFlags & kAudioFormatFlagIsFloat) != 0 &&
         (format.mFormatFlags & kAudioFormatFlagIsBigEndian) == 0 &&
         (format.mFormatFlags & kAudioFormatFlagIsNonInterleaved) == 0 &&
         format.mBitsPerChannel == 32 &&
         format.mBytesPerFrame == format.mChannelsPerFrame * sizeof(float);
}

bool supports(const std::vector<AudioValueRange> &ranges, uint32_t rate) {
  return std::any_of(ranges.begin(), ranges.end(), [rate](const AudioValueRange &range) {
    return rate >= range.mMinimum && rate <= range.mMaximum;
  });
}

uint64_t signature(const std::vector<uint32_t> &channels) {
  uint64_t result = channels.size() + 1;
  for (uint32_t count : channels) {
    result = result * 1099511628211u + count + 1;
  }
  return result;
}

void silence(AudioBufferList *list) noexcept {
  if (list == nullptr) {
    return;
  }
  for (UInt32 i = 0; i < list->mNumberBuffers; ++i) {
    if (list->mBuffers[i].mData != nullptr) {
      std::memset(list->mBuffers[i].mData, 0, list->mBuffers[i].mDataByteSize);
    }
  }
}

struct Stream {
  AudioObjectID id = 0;
  uint32_t channels = 0;
  uint32_t first = 0;
  int32_t native = 0;
  bool supported = false;
};

struct Side {
  std::vector<Stream> streams;
  uint32_t channels = 0;
  std::vector<uint32_t> counts() const {
    std::vector<uint32_t> result;
    for (const Stream &stream : streams) {
      result.push_back(stream.channels);
    }
    return result;
  }
};

struct Shape {
  Side sides[2];
  std::vector<AudioValueRange> ranges;
};

struct Route {
  uint32_t buffer = 0;
  uint32_t offset = 0;
  uint32_t stride = 0;
};

bool measure(const AudioBufferList *list, const std::vector<Route> &routes,
             uint32_t &frames) noexcept {
  for (const Route &route : routes) {
    if (list == nullptr || route.buffer >= list->mNumberBuffers) {
      return false;
    }
    const AudioBuffer &buffer = list->mBuffers[route.buffer];
    if (buffer.mData == nullptr) {
      continue;
    }
    if (buffer.mNumberChannels != route.stride) {
      return false;
    }
    const uint32_t count =
        buffer.mDataByteSize / (route.stride * static_cast<uint32_t>(sizeof(float)));
    if (frames != kUnmeasured && frames != count) {
      return false;
    }
    frames = count;
  }
  return true;
}

class CoreAudioBackend;
std::atomic<CoreAudioBackend *> route{nullptr};
std::atomic<uint32_t> route_users{0};
std::atomic_flag session = ATOMIC_FLAG_INIT;
std::atomic<uint32_t> aggregates{0};
static_assert(std::atomic<CoreAudioBackend *>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);

OSStatus notify(AudioObjectID object, UInt32 count, const AudioObjectPropertyAddress *addresses,
                void *);
OSStatus io(AudioObjectID, const AudioTimeStamp *, const AudioBufferList *input,
            const AudioTimeStamp *, AudioBufferList *output, const AudioTimeStamp *, void *);

class CoreAudioBackend final : public Backend {
public:
  explicit CoreAudioBackend(CoreAudioHal *hal) : hal_(hal == nullptr ? &system_hal : hal) {}
  ~CoreAudioBackend() override { close(); }

  const std::vector<Device> &devices() const override { return devices_; }
  ae_result refresh() override {
    std::vector<AudioObjectID> objects;
    const OSStatus status =
        list(kAudioObjectSystemObject, property(kAudioHardwarePropertyDevices), objects);
    if (status != noErr) {
      return fail("devices", status);
    }
    std::vector<Device> next;
    for (AudioObjectID object : objects) {
      const std::string identity = uid(object);
      if (identity.empty() || identity.starts_with(kAggregatePrefix)) {
        continue;
      }
      uint32_t totals[2]{};
      bool known = true;
      for (int direction = 0; direction < 2 && known; ++direction) {
        std::vector<uint32_t> channels;
        known = layout(object, direction == 0, channels) == noErr;
        for (uint32_t count : channels) {
          totals[direction] += std::min(count, AE_MAX_CHANNELS + 1);
        }
      }
      if (!known || (totals[0] == 0 && totals[1] == 0) || totals[0] > AE_MAX_CHANNELS ||
          totals[1] > AE_MAX_CHANNELS) {
        continue;
      }
      if (std::any_of(next.begin(), next.end(),
                      [&](const Device &device) { return device.device_id == identity; })) {
        continue;
      }
      std::string name = text(object, property(kAudioObjectPropertyName));
      next.push_back({"coreaudio", identity, name.empty() ? identity : name, totals[0], totals[1],
                      true, true});
    }
    devices_ = std::move(next);
    return AE_OK;
  }

  ae_result probe(const std::string &id, Capabilities &caps) override {
    if (owned_) {
      return AE_ERROR_BUSY;
    }
    diagnostic_ = {};
    AudioObjectID device = 0;
    const ae_result result = resolve(id, device);
    if (result != AE_OK) {
      return result;
    }
    Shape shape;
    return describe(device, caps, shape);
  }

  ae_result open(const OpenRequest &request, ActualFormat &actual) override {
    if (owned_) {
      return AE_ERROR_BUSY;
    }
    ae_result result;
    try {
      result = open_owned(request, actual);
    } catch (const std::bad_alloc &) {
      result = AE_ERROR_ALLOCATION_FAILURE;
    } catch (...) {
      result = AE_ERROR_DRIVER_FAILURE;
    }
    if (result != AE_OK) {
      const ae_result cleanup = close();
      if (cleanup != AE_OK) {
        return cleanup;
      }
    }
    return result;
  }

  ae_result start(BackendProcessFn process, void *state) override {
    if (!prepared_.load() || started_.load() || faults() != 0) {
      return AE_ERROR_INVALID_STATE;
    }
    process_ = process;
    state_ = state;
    frame_position_ = 0;
    callback_count_.store(0);
    max_ticks_.store(0);
    overruns_.store(0);
    overloads_.store(0);
    last_callback_.store(now());
    started_.store(true);
    gate_.store(true);
    const OSStatus status = hal_->start(device_.load(), proc_);
    if (status != noErr) {
      fail("start", status);
      if (stop_owned() != AE_OK) {
        return AE_ERROR_QUIESCENCE_FAILURE;
      }
      return AE_ERROR_DRIVER_FAILURE;
    }
    return AE_OK;
  }
  ae_result stop() override { return stop_owned(); }
  ae_result close() override { return close_owned(); }
  uint32_t faults() const noexcept override { return faults_.load(std::memory_order_acquire); }
  ae_diagnostic diagnostic() const noexcept override { return diagnostic_; }
  void observations(ae_stream_status &status) const noexcept override {
    const uint64_t revision = latency_snapshot_revision_.load();
    status.latency_valid = latency_valid_.load();
    status.input_latency_frames = input_latency_.load();
    status.output_latency_frames = output_latency_.load();
    if (revision != latency_revision_.load()) {
      status.latency_valid = 0;
    }
    status.callback_count = callback_count_.load();
    status.max_callback_nanoseconds = max_ticks_.load();
    status.deadline_overruns = overruns_.load();
    status.driver_overloads = overloads_.load();
    const uint32_t frames = frames_.load();
    const uint32_t rate = rate_.load();
    if (started_.load() && gate_.load() && rate != 0 &&
        detail::callback_stalled(last_callback_, detail::stall_timeout(kFrequency, frames, rate),
                                 [] { return now(); })) {
      faults_.fetch_or(AE_FAULT_DEVICE);
    }
  }

  void render(const AudioBufferList *input, AudioBufferList *output) noexcept {
    if (!prepared_.load()) {
      silence(output);
      return;
    }
    if (processing_.test_and_set(std::memory_order_acquire)) {
      faults_.fetch_or(AE_FAULT_CALLBACK);
      silence(output);
      return;
    }
    const uint64_t begin = now();
    last_callback_.store(begin);
    silence(output);
    uint32_t frames = kUnmeasured;
    if (!measure(input, input_routes_, frames) || !measure(output, output_routes_, frames) ||
        (frames != kUnmeasured && frames > maximum_)) {
      faults_.fetch_or(AE_FAULT_CALLBACK);
      frames = kUnmeasured;
    }
    if (frames != kUnmeasured && frames != 0 && gate_.load() && faults() == 0) {
      for (size_t i = 0; i < input_routes_.size(); ++i) {
        const Route &source = input_routes_[i];
        const auto *samples = static_cast<const float *>(input->mBuffers[source.buffer].mData);
        float *destination = input_storage_.data() + i * maximum_;
        if (samples == nullptr) {
          std::fill_n(destination, frames, 0.0f);
          continue;
        }
        samples += source.offset;
        for (uint32_t frame = 0; frame < frames; ++frame) {
          const float value = samples[static_cast<size_t>(frame) * source.stride];
          destination[frame] = std::isfinite(value) ? value : 0.0f;
        }
      }
      process_(state_, inputs_.data(), static_cast<uint32_t>(inputs_.size()), outputs_.data(),
               static_cast<uint32_t>(outputs_.size()), frames, frame_position_);
      frame_position_ += frames;
      if (gate_.load() && faults() == 0) {
        for (size_t i = 0; i < output_routes_.size(); ++i) {
          const Route &target = output_routes_[i];
          auto *samples = static_cast<float *>(output->mBuffers[target.buffer].mData);
          if (samples == nullptr) {
            continue;
          }
          samples += target.offset;
          const float *source = outputs_[i];
          for (uint32_t frame = 0; frame < frames; ++frame) {
            samples[static_cast<size_t>(frame) * target.stride] =
                std::isfinite(source[frame]) ? source[frame] : 0.0f;
          }
        }
      }
    }
    const uint64_t elapsed = now() - begin;
    callback_count_.fetch_add(1);
    if (elapsed > max_ticks_.load()) {
      max_ticks_.store(elapsed);
    }
    if (frames != kUnmeasured && elapsed > kFrequency * frames / rate_.load()) {
      overruns_.fetch_add(1);
    }
    processing_.clear(std::memory_order_release);
  }

  void notified(AudioObjectID object, UInt32 count,
                const AudioObjectPropertyAddress *addresses) noexcept {
    try {
      const AudioObjectID device = device_.load();
      for (UInt32 i = 0; i < count; ++i) {
        const AudioObjectPropertyAddress &changed = addresses[i];
        if (changed.mSelector == kAudioDevicePropertyDeviceIsAlive) {
          UInt32 alive = 0;
          if (read(object, property(kAudioDevicePropertyDeviceIsAlive), alive) != noErr ||
              alive == 0) {
            faults_.fetch_or(AE_FAULT_DEVICE);
          }
          continue;
        }
        if (object != device) {
          continue;
        }
        switch (changed.mSelector) {
        case kAudioDevicePropertyNominalSampleRate: {
          const uint32_t expected = rate_.load();
          Float64 current = 0;
          if (expected == 0) {
            break;
          }
          if (read(device, property(kAudioDevicePropertyNominalSampleRate), current) != noErr) {
            faults_.fetch_or(AE_FAULT_DEVICE);
          } else if (current != expected) {
            faults_.fetch_or(AE_FAULT_SAMPLE_RATE | (current == 0 ? AE_FAULT_CLOCK_LOST : 0));
          }
          break;
        }
        case kAudioDevicePropertyBufferFrameSize: {
          const uint32_t expected = frames_.load();
          UInt32 current = 0;
          if (expected == 0) {
            break;
          }
          if (read(device, property(kAudioDevicePropertyBufferFrameSize), current) != noErr ||
              current != expected) {
            faults_.fetch_or(AE_FAULT_RESET);
          }
          break;
        }
        case kAudioDevicePropertyStreamConfiguration:
        case kAudioDevicePropertyStreams:
          for (int direction = 0; direction < 2; ++direction) {
            const uint64_t expected = signatures_[direction].load();
            std::vector<uint32_t> channels;
            if (expected != 0 && (layout(device, direction == 0, channels) != noErr ||
                                  signature(channels) != expected)) {
              faults_.fetch_or(AE_FAULT_RESET);
            }
          }
          break;
        case kAudioDeviceProcessorOverload:
          if (started_.load()) {
            overloads_.fetch_add(1);
          }
          break;
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
          latency_revision_.fetch_add(1);
          if (prepared_.load()) {
            read_latencies();
          }
          break;
        default:
          break;
        }
      }
    } catch (...) {
      faults_.fetch_or(AE_FAULT_DEVICE);
    }
  }

private:
  ae_result fail(const char *operation, int64_t native,
                 ae_result result = AE_ERROR_DRIVER_FAILURE) {
    diagnostic_ = {};
    diagnostic_.result = result;
    diagnostic_.native_code = native;
    std::snprintf(diagnostic_.operation, sizeof(diagnostic_.operation), "%s", operation);
    std::snprintf(diagnostic_.message, sizeof(diagnostic_.message), "%s failed (native code %lld)",
                  operation, static_cast<long long>(native));
    return result;
  }
  template <typename T>
  OSStatus read(AudioObjectID object, const AudioObjectPropertyAddress &where, T &value) const {
    UInt32 bytes = sizeof(T);
    const OSStatus status = hal_->get(object, where, bytes, &value);
    if (status == noErr && bytes != sizeof(T)) {
      return kAudioHardwareBadPropertySizeError;
    }
    return status;
  }
  template <typename T>
  OSStatus list(AudioObjectID object, const AudioObjectPropertyAddress &where,
                std::vector<T> &values) const {
    UInt32 bytes = 0;
    OSStatus status = hal_->size(object, where, bytes);
    if (status != noErr) {
      return status;
    }
    values.resize(bytes / sizeof(T));
    if (values.empty()) {
      return noErr;
    }
    bytes = static_cast<UInt32>(values.size() * sizeof(T));
    status = hal_->get(object, where, bytes, values.data());
    values.resize(status == noErr ? bytes / sizeof(T) : 0);
    return status;
  }
  template <typename T, typename Predicate>
  bool await(AudioObjectID object, const AudioObjectPropertyAddress &where, uint64_t timeout,
             Predicate accepted) const {
    const uint64_t end = now() + timeout;
    for (;;) {
      T value{};
      if (read(object, where, value) == noErr && accepted(value)) {
        return true;
      }
      if (now() >= end) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  std::string text(AudioObjectID object, const AudioObjectPropertyAddress &where) const {
    CFStringRef value = nullptr;
    if (read(object, where, value) != noErr || value == nullptr) {
      return {};
    }
    Owned<CFStringRef> owned(value);
    return utf8(value);
  }
  std::string uid(AudioObjectID object) const {
    return text(object, property(kAudioDevicePropertyDeviceUID));
  }
  OSStatus layout(AudioObjectID device, bool input, std::vector<uint32_t> &channels) const {
    const auto where = property(kAudioDevicePropertyStreamConfiguration, scope(input));
    UInt32 bytes = 0;
    OSStatus status = hal_->size(device, where, bytes);
    if (status != noErr) {
      return status;
    }
    channels.clear();
    if (bytes < offsetof(AudioBufferList, mBuffers)) {
      return noErr;
    }
    std::vector<uint64_t> storage((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    status = hal_->get(device, where, bytes, storage.data());
    if (status != noErr) {
      return status;
    }
    const auto *buffers = reinterpret_cast<const AudioBufferList *>(storage.data());
    if (bytes < offsetof(AudioBufferList, mBuffers) ||
        (bytes - offsetof(AudioBufferList, mBuffers)) / sizeof(AudioBuffer) <
            buffers->mNumberBuffers) {
      return kAudioHardwareBadPropertySizeError;
    }
    for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i) {
      channels.push_back(buffers->mBuffers[i].mNumberChannels);
    }
    return noErr;
  }
  ae_result total(AudioObjectID device, bool input, uint32_t &channels) {
    std::vector<uint32_t> counts;
    const OSStatus status = layout(device, input, counts);
    if (status != noErr) {
      return fail("stream configuration", status);
    }
    uint64_t sum = 0;
    for (uint32_t count : counts) {
      sum += count;
    }
    if (sum > AE_MAX_CHANNELS) {
      return fail("channel bounds", 0);
    }
    channels = static_cast<uint32_t>(sum);
    return AE_OK;
  }
  ae_result resolve(const std::string &id, AudioObjectID &device) {
    std::vector<AudioObjectID> objects;
    const OSStatus status =
        list(kAudioObjectSystemObject, property(kAudioHardwarePropertyDevices), objects);
    if (status != noErr) {
      return fail("devices", status);
    }
    for (AudioObjectID object : objects) {
      if (uid(object) == id) {
        device = object;
        return AE_OK;
      }
    }
    return fail("device lookup", 0, AE_ERROR_DEVICE_UNAVAILABLE);
  }
  ae_result describe(AudioObjectID device, Capabilities &caps, Shape &shape) {
    caps = {};
    shape = {};
    for (int direction = 0; direction < 2; ++direction) {
      const bool input = direction == 0;
      Side &side = shape.sides[direction];
      std::vector<uint32_t> counts;
      OSStatus status = layout(device, input, counts);
      if (status != noErr) {
        return fail("stream configuration", status);
      }
      std::vector<AudioObjectID> streams;
      status = list(device, property(kAudioDevicePropertyStreams, scope(input)), streams);
      if (status != noErr) {
        return fail("streams", status);
      }
      if (streams.size() != counts.size()) {
        return fail("stream layout", 0);
      }
      auto &channels = input ? caps.inputs : caps.outputs;
      for (size_t i = 0; i < streams.size(); ++i) {
        Stream stream;
        stream.id = streams[i];
        stream.channels = counts[i];
        stream.first = side.channels;
        if (stream.channels == 0 || stream.channels > AE_MAX_CHANNELS - side.channels) {
          return fail("channel bounds", 0);
        }
        AudioStreamBasicDescription format{};
        if (read(stream.id, property(kAudioStreamPropertyVirtualFormat), format) == noErr) {
          stream.supported = float_format(format) && format.mChannelsPerFrame == stream.channels;
        }
        format = {};
        if (read(stream.id, property(kAudioStreamPropertyPhysicalFormat), format) == noErr) {
          stream.native = static_cast<int32_t>(format.mBitsPerChannel);
        }
        for (uint32_t channel = 0; channel < stream.channels; ++channel) {
          ae_channel_info info{};
          info.struct_size = sizeof(info);
          info.index = stream.first + channel;
          info.native_format = stream.native;
          info.supported = stream.supported;
          std::string name =
              text(device, property(kAudioObjectPropertyElementName, scope(input), info.index + 1));
          if (name.empty()) {
            name = (input ? "Input " : "Output ") + std::to_string(info.index + 1);
          }
          copy_name(name, info.name);
          channels.push_back(info);
        }
        side.channels += stream.channels;
        side.streams.push_back(stream);
      }
    }
    caps.info.input_channels = shape.sides[0].channels;
    caps.info.output_channels = shape.sides[1].channels;
    AudioValueRange range{};
    OSStatus status = read(device, property(kAudioDevicePropertyBufferFrameSizeRange), range);
    if (status != noErr) {
      return fail("buffer range", status);
    }
    UInt32 current = 0;
    status = read(device, property(kAudioDevicePropertyBufferFrameSize), current);
    if (status != noErr) {
      return fail("buffer size", status);
    }
    if (!std::isfinite(range.mMinimum) || !std::isfinite(range.mMaximum) || range.mMinimum < 1 ||
        range.mMaximum < range.mMinimum || range.mMinimum > AE_MAX_FRAMES_PER_BLOCK) {
      return fail("buffer bounds", 0);
    }
    caps.info.min_frames = static_cast<uint32_t>(std::ceil(range.mMinimum));
    caps.info.max_frames = static_cast<uint32_t>(
        std::floor(std::min<Float64>(range.mMaximum, AE_MAX_FRAMES_PER_BLOCK)));
    if (caps.info.max_frames < caps.info.min_frames) {
      return fail("buffer bounds", 0);
    }
    caps.info.preferred_frames = std::clamp(current, caps.info.min_frames, caps.info.max_frames);
    caps.info.frame_granularity = 1;
    Float64 rate = 0;
    status = read(device, property(kAudioDevicePropertyNominalSampleRate), rate);
    if (status != noErr) {
      return fail("sample rate", status);
    }
    if (!std::isfinite(rate) || rate < 8000 || rate > AE_MAX_SAMPLE_RATE ||
        rate != std::floor(rate)) {
      return fail("sample rate bounds", 0, AE_ERROR_UNSUPPORTED_CONFIGURATION);
    }
    caps.info.current_sample_rate = static_cast<uint32_t>(rate);
    status = list(device, property(kAudioDevicePropertyAvailableNominalSampleRates), shape.ranges);
    if (status != noErr) {
      return fail("sample rates", status);
    }
    for (uint32_t candidate : {8000u, 16000u, 22050u, 32000u, 44100u, 48000u, 88200u, 96000u,
                               176400u, 192000u, 352800u, 384000u}) {
      if (supports(shape.ranges, candidate)) {
        caps.rates.push_back(candidate);
      }
    }
    for (const AudioValueRange &discrete : shape.ranges) {
      if (discrete.mMinimum == discrete.mMaximum && discrete.mMinimum >= 8000 &&
          discrete.mMinimum <= AE_MAX_SAMPLE_RATE &&
          discrete.mMinimum == std::floor(discrete.mMinimum)) {
        const auto value = static_cast<uint32_t>(discrete.mMinimum);
        if (std::find(caps.rates.begin(), caps.rates.end(), value) == caps.rates.end()) {
          caps.rates.push_back(value);
        }
      }
    }
    std::sort(caps.rates.begin(), caps.rates.end());
    return AE_OK;
  }

  ae_result select(const ChannelSelection &selection, const Side &side, uint32_t base,
                   uint32_t limit, std::vector<uint32_t> &map, std::vector<Route> &routes,
                   std::vector<UInt32> &usage) {
    usage.assign(side.streams.size(), 0);
    if (selection.mode == AE_CHANNEL_MODE_NONE) {
      return AE_OK;
    }
    if (selection.mode == AE_CHANNEL_MODE_EXPLICIT) {
      map = selection.indices;
    } else {
      for (uint32_t i = 0; i < limit; ++i) {
        map.push_back(i);
      }
    }
    if (map.empty()) {
      return AE_ERROR_UNSUPPORTED_CONFIGURATION;
    }
    for (uint32_t index : map) {
      if (index >= limit) {
        return AE_ERROR_INVALID_ARGUMENT;
      }
      const uint32_t channel = base + index;
      const auto stream =
          std::find_if(side.streams.begin(), side.streams.end(), [channel](const Stream &entry) {
            return channel >= entry.first && channel - entry.first < entry.channels;
          });
      if (stream == side.streams.end()) {
        return fail("channel layout", channel);
      }
      if (!stream->supported) {
        return fail("sample format", stream->native, AE_ERROR_UNSUPPORTED_CONFIGURATION);
      }
      const auto buffer = static_cast<uint32_t>(stream - side.streams.begin());
      routes.push_back({buffer, channel - stream->first, stream->channels});
      usage[buffer] = 1;
    }
    return AE_OK;
  }

  ae_result combine(const std::string &input_id, const std::string &output_id,
                    AudioObjectID &device, uint32_t bases[2], uint32_t limits[2]) {
    const std::string *ids[2] = {&input_id, &output_id};
    AudioObjectID parts[2]{};
    uint32_t counts[2][2]{};
    for (int part = 0; part < 2; ++part) {
      ae_result result = resolve(*ids[part], parts[part]);
      for (int direction = 0; direction < 2 && result == AE_OK; ++direction) {
        result = total(parts[part], direction == 0, counts[part][direction]);
      }
      if (result != AE_OK) {
        return result;
      }
    }
    const uint32_t totals[2] = {counts[0][0] + counts[1][0], counts[0][1] + counts[1][1]};
    if (totals[0] > AE_MAX_CHANNELS || totals[1] > AE_MAX_CHANNELS) {
      return fail("channel bounds", 0);
    }
    limits[0] = counts[0][0];
    limits[1] = counts[1][1];
    bases[0] = 0;
    bases[1] = counts[0][1];
    const std::string identity = kAggregatePrefix + std::to_string(getpid()) + "." +
                                 std::to_string(aggregates.fetch_add(1) + 1);
    const int enabled = 1, disabled = 0;
    Owned<CFNumberRef> yes(CFNumberCreate(nullptr, kCFNumberIntType, &enabled));
    Owned<CFNumberRef> no(CFNumberCreate(nullptr, kCFNumberIntType, &disabled));
    Owned<CFMutableArrayRef> members(CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks));
    for (int part = 0; part < 2; ++part) {
      Owned<CFStringRef> member(
          CFStringCreateWithCString(nullptr, ids[part]->c_str(), kCFStringEncodingUTF8));
      Owned<CFMutableDictionaryRef> entry(CFDictionaryCreateMutable(
          nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
      CFDictionarySetValue(entry.get(), CFSTR(kAudioSubDeviceUIDKey), member.get());
      CFDictionarySetValue(entry.get(), CFSTR(kAudioSubDeviceDriftCompensationKey),
                           part == 0 ? yes.get() : no.get());
      CFArrayAppendValue(members.get(), entry.get());
    }
    Owned<CFStringRef> name(
        CFStringCreateWithCString(nullptr, "audio-io-core", kCFStringEncodingUTF8));
    Owned<CFStringRef> unique(
        CFStringCreateWithCString(nullptr, identity.c_str(), kCFStringEncodingUTF8));
    Owned<CFStringRef> clock(
        CFStringCreateWithCString(nullptr, output_id.c_str(), kCFStringEncodingUTF8));
    Owned<CFMutableDictionaryRef> description(CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    CFDictionarySetValue(description.get(), CFSTR(kAudioAggregateDeviceNameKey), name.get());
    CFDictionarySetValue(description.get(), CFSTR(kAudioAggregateDeviceUIDKey), unique.get());
    CFDictionarySetValue(description.get(), CFSTR(kAudioAggregateDeviceIsPrivateKey), yes.get());
    CFDictionarySetValue(description.get(), CFSTR(kAudioAggregateDeviceIsStackedKey), no.get());
    CFDictionarySetValue(description.get(), CFSTR(kAudioAggregateDeviceSubDeviceListKey),
                         members.get());
    CFDictionarySetValue(description.get(), CFSTR(kAudioAggregateDeviceMainSubDeviceKey),
                         clock.get());
    const OSStatus status = hal_->create_aggregate(description.get(), aggregate_);
    if (status != noErr || aggregate_ == 0) {
      aggregate_ = 0;
      return fail("create aggregate", status, AE_ERROR_DEVICE_UNAVAILABLE);
    }
    device = aggregate_;
    const uint64_t end = now() + kFrequency * 2;
    for (;;) {
      std::vector<uint32_t> channels;
      uint32_t found[2]{};
      for (int direction = 0; direction < 2; ++direction) {
        if (layout(device, direction == 0, channels) == noErr) {
          for (uint32_t count : channels) {
            found[direction] += std::min(count, AE_MAX_CHANNELS + 1);
          }
        }
      }
      if (found[0] == totals[0] && found[1] == totals[1]) {
        break;
      }
      if (now() >= end) {
        return fail("aggregate layout", 0, AE_ERROR_DEVICE_UNAVAILABLE);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (AudioObjectID part : parts) {
      const OSStatus listening = hal_->listen(part, kAnyProperty, &notify, nullptr);
      if (listening != noErr) {
        return fail("listener", listening);
      }
      listened_.push_back(part);
    }
    return AE_OK;
  }

  ae_result open_owned(const OpenRequest &request, ActualFormat &actual) {
    diagnostic_ = {};
    if (session.test_and_set()) {
      return AE_ERROR_BUSY;
    }
    owned_ = true;
    faults_.store(0);
    route.store(this);
    const bool inputs = request.inputs.mode != AE_CHANNEL_MODE_NONE;
    const bool outputs = request.outputs.mode != AE_CHANNEL_MODE_NONE;
    const std::string &output_id =
        request.output_device_id.empty() ? request.device_id : request.output_device_id;
    const bool combined = inputs && outputs && request.device_id != output_id;
    if (inputs && hal_->capture_access(true) == CaptureAccess::Denied) {
      return fail("microphone permission", 0, AE_ERROR_PERMISSION_DENIED);
    }
    AudioObjectID device = 0;
    uint32_t bases[2]{}, limits[2]{};
    ae_result result = combined ? combine(request.device_id, output_id, device, bases, limits)
                                : resolve(inputs ? request.device_id : output_id, device);
    if (result != AE_OK) {
      return result;
    }
    device_.store(device);
    OSStatus status = hal_->listen(device, kAnyProperty, &notify, nullptr);
    if (status != noErr) {
      return fail("listener", status);
    }
    listened_.push_back(device);
    Capabilities caps;
    Shape shape;
    result = describe(device, caps, shape);
    if (result != AE_OK) {
      return result;
    }
    uint32_t rate = request.sample_rate == 0 ? caps.info.current_sample_rate : request.sample_rate;
    if (!supports(shape.ranges, rate)) {
      if (!request.allow_adjustment) {
        return AE_ERROR_UNSUPPORTED_CONFIGURATION;
      }
      rate = caps.info.current_sample_rate;
    }
    if (rate != caps.info.current_sample_rate) {
      const Float64 wanted = rate;
      status = hal_->set(device, property(kAudioDevicePropertyNominalSampleRate), sizeof(wanted),
                         &wanted);
      if (status != noErr) {
        return fail("set sample rate", status);
      }
      await<Float64>(device, property(kAudioDevicePropertyNominalSampleRate), kFrequency * 5,
                     [wanted](Float64 current) { return current == wanted; });
      result = describe(device, caps, shape);
      if (result != AE_OK) {
        return result;
      }
      if (!request.allow_adjustment && caps.info.current_sample_rate != rate) {
        return fail("sample rate settle", caps.info.current_sample_rate,
                    AE_ERROR_UNSUPPORTED_CONFIGURATION);
      }
    }
    rate = caps.info.current_sample_rate;
    uint32_t frames =
        request.frames_per_block == 0 ? caps.info.preferred_frames : request.frames_per_block;
    if (frames < caps.info.min_frames || frames > caps.info.max_frames) {
      if (!request.allow_adjustment) {
        return AE_ERROR_UNSUPPORTED_CONFIGURATION;
      }
      frames = caps.info.preferred_frames;
    }
    UInt32 current = 0;
    status = read(device, property(kAudioDevicePropertyBufferFrameSize), current);
    if (status != noErr) {
      return fail("buffer size", status);
    }
    if (current != frames) {
      const UInt32 wanted = frames;
      status =
          hal_->set(device, property(kAudioDevicePropertyBufferFrameSize), sizeof(wanted), &wanted);
      if (status != noErr) {
        return fail("set buffer size", status);
      }
      await<UInt32>(device, property(kAudioDevicePropertyBufferFrameSize), kFrequency * 2,
                    [wanted](UInt32 value) { return value == wanted; });
      status = read(device, property(kAudioDevicePropertyBufferFrameSize), current);
      if (status != noErr) {
        return fail("buffer size", status);
      }
      if (current != frames) {
        if (!request.allow_adjustment) {
          return fail("buffer size settle", current, AE_ERROR_UNSUPPORTED_CONFIGURATION);
        }
        frames = current;
      }
    }
    uint32_t maximum = frames;
    UInt32 variable = 0;
    if (read(device, property(kAudioDevicePropertyUsesVariableBufferFrameSizes), variable) ==
        noErr) {
      maximum = std::max(maximum, variable);
    }
    if (frames == 0 || maximum > AE_MAX_FRAMES_PER_BLOCK) {
      return fail("buffer bounds", maximum);
    }
    if (!combined) {
      limits[0] = shape.sides[0].channels;
      limits[1] = shape.sides[1].channels;
    }
    std::vector<UInt32> usage[2];
    result = select(request.inputs, shape.sides[0], bases[0], limits[0], actual.input_map,
                    input_routes_, usage[0]);
    if (result != AE_OK) {
      return result;
    }
    result = select(request.outputs, shape.sides[1], bases[1], limits[1], actual.output_map,
                    output_routes_, usage[1]);
    if (result != AE_OK) {
      return result;
    }
    maximum_ = maximum;
    inputs_.resize(input_routes_.size());
    outputs_.resize(output_routes_.size());
    input_storage_.assign(inputs_.size() * maximum_, 0.0f);
    output_storage_.assign(outputs_.size() * maximum_, 0.0f);
    for (size_t i = 0; i < inputs_.size(); ++i) {
      inputs_[i] = input_storage_.data() + i * maximum_;
    }
    for (size_t i = 0; i < outputs_.size(); ++i) {
      outputs_[i] = output_storage_.data() + i * maximum_;
    }
    status = hal_->create_io(device, &io, nullptr, proc_);
    if (status != noErr || proc_ == nullptr) {
      proc_ = nullptr;
      return fail("create IOProc", status);
    }
    for (int direction = 0; direction < 2; ++direction) {
      latency_streams_[direction].store(0);
      for (size_t i = 0; i < usage[direction].size(); ++i) {
        if (usage[direction][i] != 0) {
          latency_streams_[direction].store(shape.sides[direction].streams[i].id);
          break;
        }
      }
      if (usage[direction].empty()) {
        continue;
      }
      std::vector<uint64_t> storage((offsetof(AudioHardwareIOProcStreamUsage, mStreamIsOn) +
                                     usage[direction].size() * sizeof(UInt32) + sizeof(uint64_t) -
                                     1) /
                                    sizeof(uint64_t));
      auto *streams = reinterpret_cast<AudioHardwareIOProcStreamUsage *>(storage.data());
      streams->mIOProc = reinterpret_cast<void *>(proc_);
      streams->mNumberStreams = static_cast<UInt32>(usage[direction].size());
      std::copy(usage[direction].begin(), usage[direction].end(), streams->mStreamIsOn);
      hal_->set(device, property(kAudioDevicePropertyIOProcStreamUsage, scope(direction == 0)),
                static_cast<UInt32>(offsetof(AudioHardwareIOProcStreamUsage, mStreamIsOn) +
                                    usage[direction].size() * sizeof(UInt32)),
                streams);
    }
    used_[0].store(inputs);
    used_[1].store(outputs);
    signatures_[0].store(signature(shape.sides[0].counts()));
    signatures_[1].store(signature(shape.sides[1].counts()));
    frames_.store(frames);
    rate_.store(rate);
    Float64 final_rate = 0;
    status = read(device, property(kAudioDevicePropertyNominalSampleRate), final_rate);
    if (status != noErr || final_rate != rate) {
      return fail("rate changed during preparation", status, AE_ERROR_CONFIGURATION_CHANGED);
    }
    status = read(device, property(kAudioDevicePropertyBufferFrameSize), current);
    if (status != noErr || current != frames) {
      return fail("buffer size changed during preparation", status, AE_ERROR_CONFIGURATION_CHANGED);
    }
    for (int direction = 0; direction < 2; ++direction) {
      std::vector<uint32_t> channels;
      status = layout(device, direction == 0, channels);
      if (status != noErr || signature(channels) != signatures_[direction].load()) {
        return fail("stream layout changed during preparation", status,
                    AE_ERROR_CONFIGURATION_CHANGED);
      }
    }
    read_latencies();
    if (faults() != 0) {
      return fail("configuration invalidated", faults(), AE_ERROR_CONFIGURATION_CHANGED);
    }
    actual.sample_rate = rate;
    actual.frames_per_block = frames;
    actual.max_frames_per_block = maximum;
    actual.input_channels = static_cast<uint32_t>(inputs_.size());
    actual.output_channels = static_cast<uint32_t>(outputs_.size());
    prepared_.store(true);
    return AE_OK;
  }

  ae_result drain() {
    const uint64_t end = now() + kFrequency * 2;
    while (route_users.load() != 0) {
      if (now() >= end) {
        return fail("callback drain", 0, AE_ERROR_QUIESCENCE_FAILURE);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return AE_OK;
  }
  ae_result stop_owned() {
    gate_.store(false);
    if (started_.load()) {
      const OSStatus status = hal_->stop(device_.load(), proc_);
      if (status != noErr && status != kAudioHardwareNotRunningError && !gone(status)) {
        return fail("stop", status, AE_ERROR_QUIESCENCE_FAILURE);
      }
      started_.store(false);
    }
    const ae_result result = drain();
    if (result != AE_OK) {
      return result;
    }
    process_ = nullptr;
    state_ = nullptr;
    return AE_OK;
  }
  ae_result close_owned() {
    if (!owned_) {
      return AE_OK;
    }
    const ae_result stopped = stop_owned();
    if (stopped != AE_OK) {
      return stopped;
    }
    prepared_.store(false);
    const ae_result drained = drain();
    if (drained != AE_OK) {
      return drained;
    }
    if (proc_ != nullptr) {
      const OSStatus status = hal_->destroy_io(device_.load(), proc_);
      if (status != noErr && !gone(status)) {
        return fail("destroy IOProc", status);
      }
      proc_ = nullptr;
    }
    for (AudioObjectID object : listened_) {
      hal_->unlisten(object, kAnyProperty, &notify, nullptr);
    }
    listened_.clear();
    route.store(nullptr);
    const ae_result detached = drain();
    if (detached != AE_OK) {
      return detached;
    }
    if (aggregate_ != 0) {
      const OSStatus status = hal_->destroy_aggregate(aggregate_);
      if (status != noErr && !gone(status)) {
        return fail("destroy aggregate", status);
      }
      aggregate_ = 0;
    }
    input_routes_.clear();
    output_routes_.clear();
    inputs_.clear();
    outputs_.clear();
    input_storage_.clear();
    output_storage_.clear();
    device_.store(0);
    rate_.store(0);
    frames_.store(0);
    signatures_[0].store(0);
    signatures_[1].store(0);
    faults_.store(0);
    latency_valid_.store(0);
    owned_ = false;
    session.clear();
    return AE_OK;
  }
  void read_latencies() noexcept {
    const uint64_t revision = latency_revision_.fetch_add(1) + 1;
    const AudioObjectID device = device_.load();
    uint32_t totals[2]{};
    bool valid = true;
    for (int direction = 0; direction < 2 && valid; ++direction) {
      if (!used_[direction].load()) {
        continue;
      }
      UInt32 latency = 0, safety = 0, stream = 0;
      const AudioObjectID source = latency_streams_[direction].load();
      valid = read(device, property(kAudioDevicePropertyLatency, scope(direction == 0)), latency) ==
                  noErr &&
              read(device, property(kAudioDevicePropertySafetyOffset, scope(direction == 0)),
                   safety) == noErr &&
              (source == 0 || read(source, property(kAudioStreamPropertyLatency), stream) == noErr);
      const uint64_t sum = uint64_t{latency} + safety + stream + frames_.load();
      valid = valid && sum <= UINT32_MAX;
      totals[direction] = static_cast<uint32_t>(sum);
    }
    if (valid) {
      input_latency_.store(totals[0]);
      output_latency_.store(totals[1]);
      latency_valid_.store(1);
      latency_snapshot_revision_.store(revision);
    } else {
      latency_valid_.store(0);
    }
  }

  CoreAudioHal *hal_;
  std::vector<Device> devices_;
  std::vector<AudioObjectID> listened_;
  AudioObjectID aggregate_ = 0;
  AudioDeviceIOProcID proc_ = nullptr;
  bool owned_ = false;
  ae_diagnostic diagnostic_{};
  uint32_t maximum_ = 0;
  uint64_t frame_position_ = 0;
  std::vector<Route> input_routes_, output_routes_;
  std::vector<float> input_storage_, output_storage_;
  std::vector<const float *> inputs_;
  std::vector<float *> outputs_;
  BackendProcessFn process_ = nullptr;
  void *state_ = nullptr;
  std::atomic<AudioObjectID> device_{0}, latency_streams_[2]{};
  std::atomic<bool> gate_{false}, prepared_{false}, started_{false}, used_[2]{};
  std::atomic_flag processing_ = ATOMIC_FLAG_INIT;
  mutable std::atomic<uint32_t> faults_{0};
  std::atomic<uint32_t> rate_{0}, frames_{0};
  std::atomic<uint32_t> latency_valid_{0}, input_latency_{0}, output_latency_{0};
  std::atomic<uint64_t> signatures_[2]{};
  std::atomic<uint64_t> latency_revision_{0}, latency_snapshot_revision_{0};
  std::atomic<uint64_t> last_callback_{0}, callback_count_{0}, max_ticks_{0}, overruns_{0},
      overloads_{0};
};

class RouteLease {
public:
  RouteLease() {
    route_users.fetch_add(1);
    backend = route.load();
  }
  ~RouteLease() { route_users.fetch_sub(1); }
  CoreAudioBackend *backend;
};

OSStatus notify(AudioObjectID object, UInt32 count, const AudioObjectPropertyAddress *addresses,
                void *) {
  RouteLease lease;
  if (lease.backend != nullptr) {
    lease.backend->notified(object, count, addresses);
  }
  return noErr;
}

OSStatus io(AudioObjectID, const AudioTimeStamp *, const AudioBufferList *input,
            const AudioTimeStamp *, AudioBufferList *output, const AudioTimeStamp *, void *) {
  RouteLease lease;
  if (lease.backend != nullptr) {
    lease.backend->render(input, output);
  } else {
    silence(output);
  }
  return noErr;
}

}

std::unique_ptr<Backend> make_coreaudio_backend(CoreAudioHal *hal) {
  return std::make_unique<CoreAudioBackend>(hal);
}
}
