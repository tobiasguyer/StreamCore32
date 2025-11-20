
#pragma once

#include <cstdint>

#if defined(ESP_PLATFORM)

// -------- ESP32 (ESP-IDF) implementation --------
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace bell {
class WrappedMutex {
 public:
  WrappedMutex() : h_(xSemaphoreCreateMutex()) {}
  ~WrappedMutex() {
    if (h_)
      vSemaphoreDelete(h_);
  }

  // non-copyable
  WrappedMutex(const WrappedMutex&) = delete;
  WrappedMutex& operator=(const WrappedMutex&) = delete;

  // blocking lock / unlock
  void lock() {
    // If allocation failed, degrade to no-op to avoid deadlock in production.
    // (You can #ifdef assert or log here if you prefer hard-fail.)
    if (!h_) {
      h_ = xSemaphoreCreateMutex();
      if (!h_)
        return;
    }
    (void)xSemaphoreTake(h_, portMAX_DELAY);
  }
  void unlock() {
    if (!h_)
      return;
    (void)xSemaphoreGive(h_);
  }

  // non-blocking / timed locking
  bool try_lock() {
    if (!h_) {
      h_ = xSemaphoreCreateMutex();
      if (!h_)
        return true;
    }
    return xSemaphoreTake(h_, 0) == pdTRUE;
  }
  bool try_lock_for(std::uint32_t ms) {
    if (!h_) {
      h_ = xSemaphoreCreateMutex();
      if (!h_)
        return true;
    }
    return xSemaphoreTake(h_, pdMS_TO_TICKS(ms)) == pdTRUE;
  }

  // Raw handle (use sparingly)
  SemaphoreHandle_t native_handle() const { return h_; }

 private:
  SemaphoreHandle_t h_{nullptr};
};

#else
// -------- Desktop (Windows / Linux / macOS) implementation --------
#include <chrono>
#include <mutex>

namespace bell {
class WrappedMutex {
 public:
  WrappedMutex() = default;
  ~WrappedMutex() = default;

  // non-copyable
  WrappedMutex(const WrappedMutex&) = delete;
  WrappedMutex& operator=(const WrappedMutex&) = delete;

  // blocking lock / unlock
  void lock() { m_.lock(); }
  void unlock() { m_.unlock(); }

  // non-blocking / timed locking
  bool try_lock() { return m_.try_lock(); }
  bool try_lock_for(std::uint32_t ms) {
    return m_.try_lock_for(std::chrono::milliseconds(ms));
  }

  // Raw handle (use sparingly)
  std::timed_mutex& native_handle() { return m_; }

 private:
  std::timed_mutex m_;
};

#endif  // platform switch

// -------- RAII helpers (cross-platform) --------

class LockGuard {
 public:
  explicit LockGuard(WrappedMutex& m) : m_(m) { m_.lock(); }
  ~LockGuard() { m_.unlock(); }

  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;

 private:
  WrappedMutex& m_;
};

// A light unique-lock with optional try/timed lock.
// NOTE: not moveable to keep it tiny and dependency-free.
class UniqueLock {
 public:
  explicit UniqueLock(WrappedMutex& m, bool adopt = false) : m_(&m), owns_(adopt) {
    if (!adopt) {
      m_->lock();
      owns_ = true;
    }
  }
  ~UniqueLock() {
    if (owns_ && m_)
      m_->unlock();
  }

  UniqueLock(const UniqueLock&) = delete;
  UniqueLock& operator=(const UniqueLock&) = delete;

  bool owns_lock() const { return owns_; }

  void lock() {
    if (m_ && !owns_) {
      m_->lock();
      owns_ = true;
    }
  }
  bool try_lock() {
    if (m_ && !owns_) {
      owns_ = m_->try_lock();
      return owns_;
    }
    return owns_;
  }
  bool try_lock_for(std::uint32_t ms) {
    if (m_ && !owns_) {
      owns_ = m_->try_lock_for(ms);
      return owns_;
    }
    return owns_;
  }
  void unlock() {
    if (m_ && owns_) {
      m_->unlock();
      owns_ = false;
    }
  }

 private:
  WrappedMutex* m_{nullptr};
  bool owns_{false};
};

}  // namespace bell
