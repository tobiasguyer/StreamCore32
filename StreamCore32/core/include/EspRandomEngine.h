#pragma once

#include <cstdint>
#include <random>
#if __has_include("esp_random.h")
#include "esp_random.h"
#else
#include "esp_system.h"
#endif

namespace streamcore {

// A replacement for std::random_device using ESP32's hardware RNG
class esp_random_device {
 public:
  using result_type = uint32_t;

  static constexpr result_type min() { return 0; }

  static constexpr result_type max() { return UINT32_MAX; }

  result_type operator()() { return esp_random(); }
};

// A random engine using esp_random_device under the hood
class esp_random_engine {
 public:
  using result_type = uint32_t;

  esp_random_engine() = default;
  explicit esp_random_engine(
      result_type /*seed*/) { /* No-op: seeded externally */ }

  static constexpr result_type min() { return 0; }

  static constexpr result_type max() { return UINT32_MAX; }

  void seed(result_type /*seed*/) {
    // Optionally reseed logic can go here if needed.
    // esp_random() doesn't allow seeding manually, so we ignore this.
  }

  result_type operator()() { return esp_random(); }
};

}  // namespace streamcore
