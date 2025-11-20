// TimeSync.h
#pragma once
#include <ctime>
#include <string>
#include <atomic>
#include <cstdint>
#include <cstdio>

#include "esp_log.h"
#include "esp_sntp.h"   // IDF v5.x
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace timesync {

static const char* TAG = "TimeSync";
static std::atomic<bool> s_started{false};

// Call once after Wi-Fi is up
inline void init(const char* s0 = "pool.ntp.org",
                 const char* s1 = "time.google.com",
                 const char* s2 = "pool.ntp.org")
{
  if (s_started.exchange(true)) return;

  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  if (s0) sntp_setservername(0, const_cast<char*>(s0));
  if (s1) sntp_setservername(1, const_cast<char*>(s1));
  if (s2) sntp_setservername(2, const_cast<char*>(s2));

  // immediate adjust on sync; IDF backs off polling by itself
  sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
  sntp_init();

  ESP_LOGI(TAG, "SNTP started: %s, %s, %s", s0, s1, s2);
}

// Block until time is valid or timeout_ms expires.
// Returns true if time is OK.
inline bool wait_until_valid(uint32_t timeout_ms = 8000) {
  const uint64_t start_ms = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
  // “Valid if later than 2019-01-01”
  const time_t min_ok = 1546300800;
  while ((static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL) - start_ms < timeout_ms) {
    time_t now = std::time(nullptr);
    if (now >= min_ok) return true;
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  return (std::time(nullptr) >= min_ok);
}

// Return UNIX epoch seconds (0 if not valid yet)
inline std::time_t now() { return std::time(nullptr); }

// NEW: epoch milliseconds (monotonic conversion of system time-of-day)
inline uint64_t now_ms() {
  // ESP-IDF provides microseconds since boot via esp_timer_get_time(), but we want epoch.
  // std::time(nullptr) gives seconds epoch; combine with gettimeofday-like precision:
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return static_cast<uint64_t>(tv.tv_sec) * 1000ULL +
         static_cast<uint64_t>(tv.tv_usec) / 1000ULL;
}

// NEW: epoch seconds as double (e.g. 1758832487.123456)
inline double now_s() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1e6;
}

// NEW: epoch seconds as a fixed-decimal text (Qobuz likes floats as text)
// decimals=6 matches common web players ("1717027415.824859")
inline std::string now_s_text(int decimals = 6) {
  if (decimals < 0) decimals = 0;
  if (decimals > 9) decimals = 9; // be sane

  struct timeval tv;
  gettimeofday(&tv, nullptr);

  char buf[48];
  if (decimals == 0) {
    std::snprintf(buf, sizeof(buf), "%ld", static_cast<long>(tv.tv_sec));
    return std::string(buf);
  }

  // pad microseconds to 6, then trim/extend to desired decimals
  char frac[16];
  std::snprintf(frac, sizeof(frac), "%06ld", static_cast<long>(tv.tv_usec)); // 6 digits
  std::string f(frac);
  if (decimals <= 6) f.resize(decimals);
  else               f.append(decimals - 6, '0');

  std::snprintf(buf, sizeof(buf), "%ld.%s", static_cast<long>(tv.tv_sec), f.c_str());
  return std::string(buf);
}


// Optional: set local timezone (so localtime() works)
// Europe/Zurich example (CET/CEST):
inline void set_timezone_ch(){
  // CET-1, CEST, rules: last Sun of Mar 02:00 -> last Sun of Oct 03:00
  setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
  tzset();
}

} // namespace timesync
