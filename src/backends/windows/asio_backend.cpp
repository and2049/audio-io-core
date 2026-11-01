#include "asio_backend.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "sample_conversion.h"

namespace ae {
namespace {

class Owner {
public:
  Owner() {
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event_ == nullptr) {
      throw std::runtime_error("CreateEvent failed");
    }
    try {
      thread_ = std::thread([this] { run(); });
    } catch (...) {
      CloseHandle(event_);
      throw;
    }
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return ready_; });
  }
  ~Owner() {
    quit_.store(true);
    SetEvent(event_);
    thread_.join();
    CloseHandle(event_);
  }
  ae_result invoke(std::function<ae_result()> function) {
    std::unique_lock lock(mutex_);
    job_ = &function;
    done_ = false;
    SetEvent(event_);
    condition_.wait(lock, [this] { return done_; });
    return result_;
  }
  HWND window() const { return window_; }
  HRESULT initialization() const { return initialization_; }
  std::function<void()> tick;

private:
  void run() {
    initialization_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(initialization_)) {
      window_ = CreateWindowExW(0, L"STATIC", L"audio-io-core", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    {
      std::lock_guard lock(mutex_);
      ready_ = true;
    }
    condition_.notify_one();
    while (!quit_.load()) {
      MsgWaitForMultipleObjects(1, &event_, FALSE, 100, QS_ALLINPUT);
      if (quit_.load()) {
        break;
      }
      std::function<ae_result()> *job;
      {
        std::lock_guard lock(mutex_);
        job = std::exchange(job_, nullptr);
      }
      if (job != nullptr) {
        ae_result result;
        try {
          result = (*job)();
        } catch (const std::bad_alloc &) {
          result = AE_ERROR_ALLOCATION_FAILURE;
        } catch (...) {
          result = AE_ERROR_DRIVER_FAILURE;
        }
        {
          std::lock_guard lock(mutex_);
          result_ = result;
          done_ = true;
        }
        condition_.notify_one();
      }
      if (tick) {
        tick();
      }
      MSG message{};
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
    }
    if (window_ != nullptr) {
      DestroyWindow(window_);
    }
    if (SUCCEEDED(initialization_)) {
      CoUninitialize();
    }
  }
  HANDLE event_ = nullptr;
  HWND window_ = nullptr;
  HRESULT initialization_ = E_FAIL;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::function<ae_result()> *job_ = nullptr;
  std::atomic<bool> quit_{false};
  bool ready_ = false;
  bool done_ = false;
  ae_result result_ = AE_OK;
};

std::string utf8(const wchar_t *value) {
  const int size =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return {};
  }
  std::string result(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(), size, nullptr,
                      nullptr);
  result.pop_back();
  return result;
}

HRESULT activate_driver(const CLSID &id, IASIO **driver) {
  return CoCreateInstance(id, nullptr, CLSCTX_INPROC_SERVER, id, reinterpret_cast<void **>(driver));
}

SampleFormat sample_format(ASIOSampleType type) {
  switch (type) {
  case ASIOSTInt16LSB:
    return {2, false, false};
  case ASIOSTInt24LSB:
    return {3, false, false};
  case ASIOSTInt32LSB:
    return {4, false, false};
  case ASIOSTFloat32LSB:
    return {4, true, false};
  case ASIOSTFloat64LSB:
    return {8, true, false};
  case ASIOSTInt16MSB:
    return {2, false, true};
  case ASIOSTInt24MSB:
    return {3, false, true};
  case ASIOSTInt32MSB:
    return {4, false, true};
  case ASIOSTFloat32MSB:
    return {4, true, true};
  case ASIOSTFloat64MSB:
    return {8, true, true};
  default:
    return {};
  }
}

bool valid_frames(uint32_t frames, const ae_device_capabilities &caps) {
  if (frames < caps.min_frames || frames > caps.max_frames) {
    return false;
  }
  if (caps.frame_granularity == -1) {
    return (frames & (frames - 1)) == 0;
  }
  if (caps.frame_granularity == 0) {
    return frames == caps.preferred_frames;
  }
  return (frames - caps.min_frames) % static_cast<uint32_t>(caps.frame_granularity) == 0;
}

class AsioBackend;
std::atomic<AsioBackend *> route{nullptr};
std::atomic<uint32_t> route_users{0};
std::atomic_flag session = ATOMIC_FLAG_INIT;
static_assert(std::atomic<AsioBackend *>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);

class AsioBackend final : public Backend {
public:
  AsioBackend(AsioFactory factory, std::vector<Device> devices)
      : factory_(factory == nullptr ? activate_driver : factory), injected_(factory != nullptr),
        devices_(std::move(devices)) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    frequency_ = static_cast<uint64_t>(frequency.QuadPart);
    owner_.invoke([this] {
      owner_.tick = [this] { service(); };
      return AE_OK;
    });
  }
  ~AsioBackend() override {
    owner_.invoke([this] {
      owner_.tick = {};
      return AE_OK;
    });
  }

  const std::vector<Device> &devices() const override { return devices_; }
  ae_result refresh() override {
    if (injected_) {
      return AE_OK;
    }
    return owner_.invoke([this]() -> ae_result {
      HKEY key = nullptr;
      const LSTATUS opened =
          RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ | KEY_WOW64_64KEY, &key);
      if (opened == ERROR_FILE_NOT_FOUND) {
        devices_.clear();
        return AE_OK;
      }
      if (opened != ERROR_SUCCESS) {
        return fail("registry", opened);
      }
      std::vector<Device> next;
      try {
        for (DWORD index = 0;; ++index) {
          wchar_t name[256]{};
          DWORD length = 256;
          const LSTATUS result =
              RegEnumKeyExW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr);
          if (result == ERROR_NO_MORE_ITEMS) {
            break;
          }
          if (result != ERROR_SUCCESS) {
            RegCloseKey(key);
            return fail("registry enumerate", result);
          }
          wchar_t text[128]{};
          DWORD bytes = sizeof(text);
          if (RegGetValueW(key, name, L"CLSID", RRF_RT_REG_SZ, nullptr, text, &bytes) !=
              ERROR_SUCCESS) {
            continue;
          }
          CLSID id{};
          if (FAILED(CLSIDFromString(text, &id))) {
            continue;
          }
          wchar_t canonical[40]{};
          StringFromGUID2(id, canonical, 40);
          const std::string identity = utf8(canonical);
          if (std::none_of(next.begin(), next.end(),
                           [&](const Device &device) { return device.device_id == identity; })) {
            next.push_back({"asio", identity, utf8(name), 0, 0, false});
          }
        }
      } catch (...) {
        RegCloseKey(key);
        throw;
      }
      RegCloseKey(key);
      devices_ = std::move(next);
      return AE_OK;
    });
  }

  ae_result probe(const std::string &id, Capabilities &caps) override {
    if (owned_) {
      return AE_ERROR_BUSY;
    }
    ae_result result = owner_.invoke([&] {
      const auto activated = activate(id);
      return activated == AE_OK ? capabilities(caps) : activated;
    });
    const ae_result cleanup = close();
    return cleanup == AE_OK ? result : cleanup;
  }

  ae_result open(const OpenRequest &request, ActualFormat &actual) override {
    if (owned_) {
      return AE_ERROR_BUSY;
    }
    ae_result result = owner_.invoke([&] { return open_owned(request, actual); });
    if (result != AE_OK) {
      const ae_result cleanup = close();
      if (cleanup != AE_OK) {
        return cleanup;
      }
    }
    return result;
  }

  ae_result start(BackendProcessFn process, void *state) override {
    return owner_.invoke([&] {
      if (!prepared_.load() || started_ || faults() != 0) {
        return AE_ERROR_INVALID_STATE;
      }
      clear_outputs(0);
      clear_outputs(1);
      process_ = process;
      state_ = state;
      frame_position_ = 0;
      callback_count_.store(0);
      max_ticks_.store(0);
      overruns_.store(0);
      overloads_.store(0);
      last_callback_.store(now());
      started_ = true;
      gate_.store(true);
      ASIOError result;
      try {
        result = driver_->start();
      } catch (...) {
        result = ASE_HWMalfunction;
      }
      if (result != ASE_OK) {
        fail("start", result);
        if (stop_owned() != AE_OK) {
          return AE_ERROR_QUIESCENCE_FAILURE;
        }
        return AE_ERROR_DRIVER_FAILURE;
      }
      return AE_OK;
    });
  }
  ae_result stop() override {
    return owner_.invoke([this] { return stop_owned(); });
  }
  ae_result close() override {
    return owner_.invoke([this] { return close_owned(); });
  }
  uint32_t faults() const noexcept override { return faults_.load(std::memory_order_acquire); }
  ae_diagnostic diagnostic() const noexcept override { return diagnostic_; }
  void observations(ae_stream_status &status) const noexcept override {
    status.latency_valid = latency_valid_.load();
    status.input_latency_frames = input_latency_.load();
    status.output_latency_frames = output_latency_.load();
    status.callback_count = callback_count_.load();
    status.max_callback_nanoseconds = ticks_to_ns(max_ticks_.load());
    status.deadline_overruns = overruns_.load();
    status.driver_overloads = overloads_.load();
  }

  void buffer(long index, ASIOBool direct) noexcept {
    if (!prepared_.load()) {
      return;
    }
    if (processing_.test_and_set(std::memory_order_acquire)) {
      faults_.fetch_or(AE_FAULT_CALLBACK);
      return;
    }
    const uint64_t begin = now();
    last_callback_.store(begin);
    if (index < 0 || index > 1) {
      faults_.fetch_or(AE_FAULT_CALLBACK);
      processing_.clear(std::memory_order_release);
      return;
    }
    if (direct == ASIOFalse) {
      faults_.fetch_or(AE_FAULT_CALLBACK);
    }
    try {
      if (gate_.load() && faults() == 0) {
        for (size_t i = 0; i < inputs_.size(); ++i) {
          decode_samples(formats_[i], buffers_[i].buffers[index],
                         input_storage_.data() + i * frames_, frames_);
        }
        std::fill(output_storage_.begin(), output_storage_.end(), 0.0f);
        process_(state_, inputs_.data(), static_cast<uint32_t>(inputs_.size()), outputs_.data(),
                 static_cast<uint32_t>(outputs_.size()), frames_, frame_position_);
        frame_position_ += frames_;
        if (!gate_.load() || faults() != 0) {
          std::fill(output_storage_.begin(), output_storage_.end(), 0.0f);
        }
        for (size_t i = 0; i < outputs_.size(); ++i) {
          encode_samples(formats_[inputs_.size() + i], outputs_[i],
                         buffers_[inputs_.size() + i].buffers[index], frames_);
        }
      } else {
        clear_outputs(index);
      }
      if (output_ready_) {
        if (driver_->outputReady() != ASE_OK) {
          faults_.fetch_or(AE_FAULT_DEVICE);
        }
      }
    } catch (...) {
      faults_.fetch_or(AE_FAULT_CALLBACK);
      clear_outputs(index);
    }
    const uint64_t elapsed = now() - begin;
    callback_count_.fetch_add(1);
    if (elapsed > max_ticks_.load()) {
      max_ticks_.store(elapsed);
    }
    if (elapsed > frequency_ * frames_ / rate_) {
      overruns_.fetch_add(1);
    }
    processing_.clear(std::memory_order_release);
  }

  long message(long selector, long value) noexcept {
    switch (selector) {
    case kAsioSelectorSupported:
      return value == kAsioResetRequest || value == kAsioResyncRequest ||
             value == kAsioBufferSizeChange || value == kAsioLatenciesChanged ||
             value == kAsioEngineVersion || value == kAsioSupportsTimeInfo ||
             value == kAsioOverload;
    case kAsioEngineVersion:
      return 2;
    case kAsioSupportsTimeInfo:
      return 0;
    case kAsioResetRequest:
    case kAsioBufferSizeChange:
      faults_.fetch_or(AE_FAULT_RESET);
      return 1;
    case kAsioResyncRequest:
      faults_.fetch_or(AE_FAULT_RESYNC);
      return 1;
    case kAsioLatenciesChanged:
      latency_valid_.store(0);
      latency_dirty_.store(true);
      return 1;
    case kAsioOverload:
      overloads_.fetch_add(1);
      return 1;
    default:
      return 0;
    }
  }
  void rate_changed(ASIOSampleRate rate) noexcept {
    faults_.fetch_or(AE_FAULT_SAMPLE_RATE | (rate == 0 ? AE_FAULT_CLOCK_LOST : 0));
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
  uint64_t now() const noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<uint64_t>(value.QuadPart);
  }
  uint64_t ticks_to_ns(uint64_t ticks) const noexcept {
    return static_cast<uint64_t>(static_cast<double>(ticks) *
                                 (1.0e9 / static_cast<double>(frequency_)));
  }
  ae_result drain() {
    const uint64_t end = now() + frequency_ * 2;
    while (route_users.load() != 0) {
      if (now() >= end) {
        return fail("callback drain", 0, AE_ERROR_QUIESCENCE_FAILURE);
      }
      Sleep(1);
    }
    return AE_OK;
  }
  ae_result activate(const std::string &id) {
    diagnostic_ = {};
    if (FAILED(owner_.initialization()) || owner_.window() == nullptr) {
      return fail("COM/window initialization", owner_.initialization());
    }
    if (session.test_and_set()) {
      return AE_ERROR_BUSY;
    }
    owned_ = true;
    faults_.store(0);
    route.store(this);
    std::wstring wide(id.begin(), id.end());
    CLSID clsid{};
    HRESULT result = CLSIDFromString(wide.c_str(), &clsid);
    if (FAILED(result)) {
      return fail("driver identity", result, AE_ERROR_INVALID_ARGUMENT);
    }
    result = factory_(clsid, &driver_);
    if (FAILED(result) || driver_ == nullptr) {
      return fail("CoCreateInstance", result, AE_ERROR_DEVICE_UNAVAILABLE);
    }
    if (driver_->init(owner_.window()) == ASIOFalse) {
      return fail("init", 0, AE_ERROR_DEVICE_UNAVAILABLE);
    }
    return AE_OK;
  }
  ae_result capabilities(Capabilities &caps) {
    caps = {};
    long inputs = 0, outputs = 0;
    ASIOError result = driver_->getChannels(&inputs, &outputs);
    if (result != ASE_OK) {
      return fail("getChannels", result);
    }
    if (inputs < 0 || outputs < 0 || inputs > static_cast<long>(AE_MAX_CHANNELS) ||
        outputs > static_cast<long>(AE_MAX_CHANNELS)) {
      return fail("channel bounds", 0);
    }
    caps.info.input_channels = static_cast<uint32_t>(inputs);
    caps.info.output_channels = static_cast<uint32_t>(outputs);
    long minimum = 0, maximum = 0, preferred = 0, granularity = 0;
    result = driver_->getBufferSize(&minimum, &maximum, &preferred, &granularity);
    if (result != ASE_OK) {
      return fail("getBufferSize", result);
    }
    if (minimum < 1 || maximum < minimum || maximum > static_cast<long>(AE_MAX_FRAMES_PER_BLOCK) ||
        preferred < minimum || preferred > maximum || granularity < -1) {
      return fail("buffer bounds", 0);
    }
    caps.info.min_frames = static_cast<uint32_t>(minimum);
    caps.info.max_frames = static_cast<uint32_t>(maximum);
    caps.info.preferred_frames = static_cast<uint32_t>(preferred);
    caps.info.frame_granularity = granularity;
    if (granularity == 0 && minimum != maximum) {
      return fail("fixed buffer bounds", 0);
    }
    if (!valid_frames(caps.info.preferred_frames, caps.info)) {
      return fail("preferred buffer", 0);
    }
    ASIOSampleRate rate = 0;
    result = driver_->getSampleRate(&rate);
    if (result != ASE_OK) {
      return fail("getSampleRate", result);
    }
    if (!std::isfinite(rate) || rate < 8000 || rate > AE_MAX_SAMPLE_RATE ||
        rate != std::floor(rate)) {
      return fail("sample rate bounds", 0, AE_ERROR_UNSUPPORTED_CONFIGURATION);
    }
    caps.info.current_sample_rate = static_cast<uint32_t>(rate);
    for (uint32_t candidate : {8000u, 16000u, 22050u, 32000u, 44100u, 48000u, 88200u, 96000u,
                               176400u, 192000u, 352800u, 384000u}) {
      if (driver_->canSampleRate(candidate) == ASE_OK) {
        caps.rates.push_back(candidate);
      }
    }
    for (int direction = 0; direction < 2; ++direction) {
      auto &channels = direction == 0 ? caps.inputs : caps.outputs;
      const long count = direction == 0 ? inputs : outputs;
      for (long index = 0; index < count; ++index) {
        ASIOChannelInfo native{};
        native.channel = index;
        native.isInput = direction == 0 ? ASIOTrue : ASIOFalse;
        result = driver_->getChannelInfo(&native);
        if (result != ASE_OK) {
          return fail("getChannelInfo", result);
        }
        ae_channel_info info{};
        info.struct_size = sizeof(info);
        info.index = static_cast<uint32_t>(index);
        info.native_format = native.type;
        info.supported = valid_format(sample_format(native.type));
        std::memcpy(info.name, native.name, sizeof(native.name));
        info.name[sizeof(native.name)] = '\0';
        channels.push_back(info);
      }
    }
    return AE_OK;
  }

  ae_result select(const ChannelSelection &selection, const std::vector<ae_channel_info> &channels,
                   bool input, std::vector<uint32_t> &map) {
    if (selection.mode == AE_CHANNEL_MODE_NONE) {
      return AE_OK;
    }
    if (selection.mode == AE_CHANNEL_MODE_EXPLICIT) {
      map = selection.indices;
    } else {
      for (uint32_t i = 0; i < channels.size(); ++i) {
        map.push_back(i);
      }
    }
    if (map.empty()) {
      return AE_ERROR_UNSUPPORTED_CONFIGURATION;
    }
    for (uint32_t index : map) {
      if (index >= channels.size()) {
        return AE_ERROR_INVALID_ARGUMENT;
      }
      const auto format = sample_format(channels[index].native_format);
      if (!valid_format(format)) {
        return fail("sample format", channels[index].native_format,
                    AE_ERROR_UNSUPPORTED_CONFIGURATION);
      }
      ASIOBufferInfo buffer{};
      buffer.isInput = input ? ASIOTrue : ASIOFalse;
      buffer.channelNum = static_cast<long>(index);
      buffers_.push_back(buffer);
      formats_.push_back(format);
    }
    return AE_OK;
  }

  ae_result open_owned(const OpenRequest &request, ActualFormat &actual);
  ae_result stop_owned() {
    gate_.store(false);
    if (started_) {
      ASIOError result;
      try {
        result = driver_->stop();
      } catch (...) {
        return fail("stop exception", 0, AE_ERROR_QUIESCENCE_FAILURE);
      }
      if (result != ASE_OK) {
        return fail("stop", result, AE_ERROR_QUIESCENCE_FAILURE);
      }
      started_ = false;
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
    if (buffers_created_) {
      const ASIOError result = driver_->disposeBuffers();
      if (result != ASE_OK) {
        return fail("disposeBuffers", result);
      }
      buffers_created_ = false;
    }
    route.store(nullptr);
    const ae_result detached = drain();
    if (detached != AE_OK) {
      return detached;
    }
    if (driver_ != nullptr) {
      driver_->Release();
      driver_ = nullptr;
    }
    buffers_.clear();
    formats_.clear();
    inputs_.clear();
    outputs_.clear();
    input_storage_.clear();
    output_storage_.clear();
    faults_.store(0);
    latency_valid_.store(0);
    owned_ = false;
    session.clear();
    return AE_OK;
  }
  void clear_outputs(long index) noexcept {
    for (size_t i = inputs_.size(); i < buffers_.size(); ++i) {
      std::memset(buffers_[i].buffers[index], 0, static_cast<size_t>(frames_) * formats_[i].bytes);
    }
  }
  void read_latencies() {
    long input = 0, output = 0;
    if (driver_->getLatencies(&input, &output) == ASE_OK && input >= 0 && output >= 0) {
      input_latency_.store(static_cast<uint32_t>(input));
      output_latency_.store(static_cast<uint32_t>(output));
      latency_valid_.store(1);
    } else {
      latency_valid_.store(0);
    }
  }
  void service() noexcept {
    try {
      if (prepared_.load() && latency_dirty_.exchange(false)) {
        read_latencies();
      }
      if (started_ && gate_.load() && now() - last_callback_.load() > frequency_ * 2) {
        faults_.fetch_or(AE_FAULT_DEVICE);
      }
    } catch (...) {
      faults_.fetch_or(AE_FAULT_DEVICE);
    }
  }

  AsioFactory factory_;
  bool injected_;
  std::vector<Device> devices_;
  IASIO *driver_ = nullptr;
  bool owned_ = false;
  bool started_ = false;
  bool buffers_created_ = false;
  bool output_ready_ = false;
  ASIOCallbacks callbacks_{};
  ae_diagnostic diagnostic_{};
  uint32_t frames_ = 0;
  uint32_t rate_ = 0;
  uint64_t frequency_ = 1;
  uint64_t frame_position_ = 0;
  std::vector<ASIOBufferInfo> buffers_;
  std::vector<SampleFormat> formats_;
  std::vector<float> input_storage_, output_storage_;
  std::vector<const float *> inputs_;
  std::vector<float *> outputs_;
  BackendProcessFn process_ = nullptr;
  void *state_ = nullptr;
  std::atomic<bool> gate_{false}, prepared_{false}, latency_dirty_{false};
  std::atomic_flag processing_ = ATOMIC_FLAG_INIT;
  std::atomic<uint32_t> faults_{0}, latency_valid_{0}, input_latency_{0}, output_latency_{0};
  std::atomic<uint64_t> last_callback_{0}, callback_count_{0}, max_ticks_{0}, overruns_{0},
      overloads_{0};
  Owner owner_;
};

class RouteLease {
public:
  RouteLease() {
    route_users.fetch_add(1);
    backend = route.load();
  }
  ~RouteLease() { route_users.fetch_sub(1); }
  AsioBackend *backend;
};

void buffer_switch(long index, ASIOBool direct) {
  RouteLease lease;
  if (lease.backend != nullptr) {
    lease.backend->buffer(index, direct);
  }
}
ASIOTime *time_switch(ASIOTime *, long index, ASIOBool direct) {
  buffer_switch(index, direct);
  return nullptr;
}
void sample_rate_changed(ASIOSampleRate rate) {
  RouteLease lease;
  if (lease.backend != nullptr) {
    lease.backend->rate_changed(rate);
  }
}
long asio_message(long selector, long value, void *, double *) {
  RouteLease lease;
  return lease.backend == nullptr ? 0 : lease.backend->message(selector, value);
}

ae_result AsioBackend::open_owned(const OpenRequest &request, ActualFormat &actual) {
  ae_result result = activate(request.device_id);
  if (result != AE_OK) {
    return result;
  }
  Capabilities caps;
  result = capabilities(caps);
  if (result != AE_OK) {
    return result;
  }
  uint32_t rate = request.sample_rate == 0 ? caps.info.current_sample_rate : request.sample_rate;
  if (driver_->canSampleRate(rate) != ASE_OK) {
    if (!request.allow_adjustment) {
      return AE_ERROR_UNSUPPORTED_CONFIGURATION;
    }
    rate = caps.info.current_sample_rate;
  }
  if (rate != caps.info.current_sample_rate) {
    const ASIOError changed = driver_->setSampleRate(rate);
    if (changed != ASE_OK) {
      return fail("setSampleRate", changed);
    }
  }
  faults_.fetch_and(~uint32_t{AE_FAULT_SAMPLE_RATE});
  result = capabilities(caps);
  if (result != AE_OK) {
    return result;
  }
  if (!request.allow_adjustment && caps.info.current_sample_rate != rate) {
    return AE_ERROR_UNSUPPORTED_CONFIGURATION;
  }
  rate_ = caps.info.current_sample_rate;
  frames_ = request.frames_per_block == 0 ? caps.info.preferred_frames : request.frames_per_block;
  if (!valid_frames(frames_, caps.info)) {
    if (!request.allow_adjustment) {
      return AE_ERROR_UNSUPPORTED_CONFIGURATION;
    }
    frames_ = caps.info.preferred_frames;
  }
  result = select(request.inputs, caps.inputs, true, actual.input_map);
  if (result != AE_OK) {
    return result;
  }
  result = select(request.outputs, caps.outputs, false, actual.output_map);
  if (result != AE_OK) {
    return result;
  }
  inputs_.resize(actual.input_map.size());
  outputs_.resize(actual.output_map.size());
  input_storage_.resize(inputs_.size() * frames_);
  output_storage_.resize(outputs_.size() * frames_);
  for (size_t i = 0; i < inputs_.size(); ++i) {
    inputs_[i] = input_storage_.data() + i * frames_;
  }
  for (size_t i = 0; i < outputs_.size(); ++i) {
    outputs_[i] = output_storage_.data() + i * frames_;
  }
  callbacks_ = {buffer_switch, sample_rate_changed, asio_message, time_switch};
  buffers_created_ = true;
  const ASIOError created = driver_->createBuffers(
      buffers_.data(), static_cast<long>(buffers_.size()), static_cast<long>(frames_), &callbacks_);
  if (created != ASE_OK) {
    return fail("createBuffers", created);
  }
  for (const auto &buffer : buffers_) {
    if (buffer.buffers[0] == nullptr || buffer.buffers[1] == nullptr) {
      return fail("buffer pointers", 0);
    }
  }
  ASIOSampleRate final_rate = 0;
  const ASIOError queried = driver_->getSampleRate(&final_rate);
  if (queried != ASE_OK || final_rate != rate_) {
    return fail("rate changed during buffer creation", queried, AE_ERROR_UNSUPPORTED_CONFIGURATION);
  }
  for (size_t i = 0; i < buffers_.size(); ++i) {
    const bool input = i < actual.input_map.size();
    const uint32_t expected =
        input ? actual.input_map[i] : actual.output_map[i - actual.input_map.size()];
    if (buffers_[i].channelNum != static_cast<long>(expected) ||
        buffers_[i].isInput != (input ? ASIOTrue : ASIOFalse)) {
      return fail("channel mapping changed during buffer creation", 0);
    }
    ASIOChannelInfo channel{};
    channel.channel = static_cast<long>(expected);
    channel.isInput = input ? ASIOTrue : ASIOFalse;
    const ASIOError channel_result = driver_->getChannelInfo(&channel);
    if (channel_result != ASE_OK) {
      return fail("getChannelInfo after buffer creation", channel_result);
    }
    const auto &previous = input ? caps.inputs[expected] : caps.outputs[expected];
    if (channel.type != previous.native_format) {
      return fail("sample format changed during buffer creation", channel.type,
                  AE_ERROR_CONFIGURATION_CHANGED);
    }
  }
  clear_outputs(0);
  clear_outputs(1);
  output_ready_ = driver_->outputReady() == ASE_OK;
  read_latencies();
  if (faults() != 0) {
    return fail("configuration invalidated", faults(), AE_ERROR_CONFIGURATION_CHANGED);
  }
  actual.sample_rate = rate_;
  actual.frames_per_block = frames_;
  actual.max_frames_per_block = frames_;
  actual.input_channels = static_cast<uint32_t>(inputs_.size());
  actual.output_channels = static_cast<uint32_t>(outputs_.size());
  prepared_.store(true);
  return AE_OK;
}

}

std::unique_ptr<Backend> make_asio_backend(AsioFactory factory, std::vector<Device> devices) {
  return std::make_unique<AsioBackend>(factory, std::move(devices));
}
}
