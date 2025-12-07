#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "BellTask.h"
#include "HTTPClient.h"
#include "Logger.h"
#include "StreamBase.h"
#include "UrlOrigin.h"

class MetaPoller : public bell::Task {
 public:
  enum class Kind { Auto, IcecastJSON, ShoutcastJSON, Shoutcast7, Disabled };

  struct Spec {
    Kind kind = Kind::Auto;
    std::string url;  // optional explicit endpoint (abs or relative)
    uint32_t intervalMs = 5000;
    bool enabled = true;
  };

  using Emit = std::function<void(const std::string&, const std::string&)>;
  using Err = std::function<void(const std::string&)>;

  explicit MetaPoller(Emit e, Err er)
      : bell::Task("MetaPoller", 4096 * 4, 0, 1),
        emit_(std::move(e)),
        err_(std::move(er)) {}

  ~MetaPoller() {
    while (isRunning_.load())
      BELL_SLEEP_MS(100);
  }
  void arm(const std::string& origin, const std::string& station,
           const Spec& spec) {
    std::lock_guard<std::mutex> lk(mu_);
    origin_ = origin;
    station_ = station;
    spec_ = spec;
    active_.store(true);
  }
  void disarm() { active_.store(false); }
  void stopTask() { wantStop_.store(true); }
  bool isRunning() const { return isRunning_.load(); }

 protected:
  void runTask() override;

 private:
  using json = nlohmann::json;
  std::unique_ptr<bell::HTTPClient::Response> httpGetSimple(
      const std::string& url);
  static int statusFromHeaders(bell::HTTPClient::Response& r);
  static size_t sizeFromHeader(std::string_view sv);
  static std::string pickIcecastTitle(const json& s);
  static std::string parseShoutcast7(std::string_view);
  static int toInt(std::string_view sv) {
    int v = 0;
    for (char c : sv)
      if (std::isdigit((unsigned char)c))
        v = v * 10 + (c - '0');
    return v;
  }

  std::mutex mu_;
  std::atomic<bool> active_{false};
  std::atomic<bool> isRunning_{false};
  std::atomic<bool> wantStop_{false};
  Spec spec_{};
  std::string origin_;
  std::string station_;
  std::string lastTitle_;

  // Sticky good endpoint
  std::string lockedUrl_;
  int lockedFailures_ = 0;  // clear after 3 consecutive bad polls

  Emit emit_;
  Err err_;
};
