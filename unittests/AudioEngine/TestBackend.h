#ifndef AUDIO_ENGINE_UNITTESTS_TESTBACKEND_H
#define AUDIO_ENGINE_UNITTESTS_TESTBACKEND_H

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "backend.h"

class TestBackend final : public ae::Backend {
public:
  enum class Failure { None, Error, Exception, Allocation };

  TestBackend();
  ~TestBackend() override;

  TestBackend(const TestBackend &) = delete;
  TestBackend &operator=(const TestBackend &) = delete;

  const std::vector<ae::Device> &devices() const override;
  ae_result open(const ae::OpenRequest &request, ae::ActualFormat &actual) override;
  ae_result start(ae::BackendProcessFn process, void *engine_state) override;
  ae_result stop() override;
  void close() noexcept override;

  void setIgnoreExactNegotiation(bool value) {
    ignore_exact_negotiation_.store(value, std::memory_order_relaxed);
  }
  void setStartFailure(Failure failure) { start_failure_ = failure; }
  void setStopFailure(Failure failure) { stop_failure_ = failure; }

private:
  void run();

  std::vector<ae::Device> devices_;
  ae::ActualFormat actual_{};
  ae::BackendProcessFn process_ = nullptr;
  void *engine_state_ = nullptr;
  std::thread thread_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> ignore_exact_negotiation_{false};
  Failure start_failure_ = Failure::None;
  Failure stop_failure_ = Failure::None;
  uint64_t frame_position_ = 0;
};

#endif
