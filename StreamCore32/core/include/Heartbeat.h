// Heartbeat.h
#pragma once
#include <atomic>
#include <functional>
#include "BellLogger.h"  // optional
#include "BellTask.h"    // bell::task(...)
#include "BellUtils.h"

class Heartbeat : public bell::Task {
 public:
  using SendFn = std::function<void()>;

  explicit Heartbeat(SendFn send, uint32_t timeout_ms = 10000)
      : bell::Task("Heartbeat", 1024 * 8, 1, 1, 1),
        send_(std::move(send)),
        timeout_ms_(timeout_ms) {}
  ~Heartbeat() { stop(); }

  void start() {
    if (running_.exchange(true))
      return;  // already running
    stopFlag_.store(false);
    startTask();
  }

  void stop() {
    stopFlag_.store(true);
    while (running_.load())
      BELL_SLEEP_MS(100);
  }

  void delay(uint32_t ms = 10000) {
    std::scoped_lock lock(delay_mutex_);
    delay_ = std::min(timeout_ms_, delay_ + ms);
  }

  bool running() const { return running_.load(); }

  void runTask() override {
    while (!stopFlag_.load()) {
      if (send_) {
        send_();
      }
      delay_ = timeout_ms_;
      while (delay_ > 0) {
        BELL_SLEEP_MS(100);
        std::scoped_lock lock(delay_mutex_);
        delay_ -= 100;
        if (stopFlag_.load())
          break;
      }
    }
    running_.store(false);
  }

 private:
  SendFn send_;
  std::atomic<bool> stopFlag_{false};
  std::atomic<bool> running_{false};
  std::mutex delay_mutex_;
  uint32_t delay_ = 0;
  uint32_t timeout_ms_ = 10000;
};
