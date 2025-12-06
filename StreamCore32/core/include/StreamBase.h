#pragma once
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "BellTask.h"
#include "AudioControl.h"
#include "HTTPClient.h"
#include "Logger.h"
#include "StreamCoreFile.h"

class StreamBase : public bell::Task {
public:
  using MetaCb = std::function<void(const std::string&, const std::string&)>;
  using ErrorCb = std::function<void(const std::string&)>;
  using StateCb = std::function<void(bool)>;

  explicit StreamBase(const char* taskName,
    std::shared_ptr<AudioControl> audio,
    int stackSize = 1024 * 16,
    int prio = 1,
    int core = 1,
    bool runOnPSRAM = true
  ) : bell::Task(taskName, stackSize, prio, core, runOnPSRAM)
  {
    audio_ = audio;
    feed_ = std::make_shared<AudioControl::FeedControl>(audio_);
  }

  virtual ~StreamBase() {}

  // Common callbacks
  void onMetadata(MetaCb cb) { onMeta_ = std::move(cb); }
  void onError(ErrorCb cb) { onError_ = std::move(cb); }
  void onState(StateCb cb) { onState_ = std::move(cb); }

  // Start playback of a URI (meaning depends on derived stream)
  virtual void play(const std::string& uri, const std::string& displayName = std::string()) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      targetUri_ = uri;
      displayName_ = displayName;
    }
    wantStop_.store(false);
    wantRestart_.store(true);
    if (!isRunning_.load()) startTask();
  }

  // Stop and flush/close sink
  virtual void stop() {
    wantStop_.store(true);
    if (feed_) {
      feed_->feedCommand(AudioControl::FLUSH, 0);
      feed_->feedCommand(AudioControl::DISC, 0);
    }
  }

  bool isRunning() const { return isRunning_.load(); }

  virtual std::unique_ptr<bell::HTTPClient::Response>
    open(const std::string& uri, const std::string& displayName, uint32_t tid) {
    (void)tid;
    if (!displayName.empty()) emitMeta(displayName, "");
    auto resp = bell::HTTPClient::get(uri);     // uses SocketStream under the hood
    if (!resp || resp->contentLength() == 0) {  // len may be 0 for live streams; still ok
      reportError("DLNA: failed to open " + uri);
      return nullptr;
    }
    return resp; // StreamBase::runTask() will pump resp->stream()
  }

  virtual int read(bell::SocketStream& is, uint8_t* dst, int n, uint32_t tid) {
    is.read(reinterpret_cast<char*>(dst), n);
    return is.gcount();
  }

  void runTask() override {

    isRunning_.store(true);

    while (isRunning_.load()) {
      if (wantStop_.load()) { isRunning_.store(false); break; }
      if (!wantRestart_.load()) { sleepMs(25); continue; }

      std::string uri; std::string name;
      {
        std::lock_guard<std::mutex> lk(mu_);
        uri = targetUri_;
        name = displayName_;
      }
      wantRestart_.store(false);
      if (uri.empty()) { sleepMs(100); continue; }

      // Bump track id so the sink treats this as a fresh stream and soft-stops previous

      const uint32_t tid = audio_->makeUniqueTrackId();
      if (onState_) onState_(true);
      auto resp = open(targetUri_, name, tid);
      if (resp == nullptr) {
        isRunning_.store(false);
        break;
      }
      const size_t CHUNK = 1024; uint8_t buf[CHUNK]; int ret = 0;
      while (!wantStop_.load()) {
        ret = read(resp->stream(), buf, 1024, tid);
        if (ret <= 0) {
          BELL_LOG(debug, "Qobuz", "read %d bytes", ret);
          break;
        }
        else BELL_LOG(debug, "Qobuz", "read %d bytes", ret);
        uint16_t written = 0;
        while (written < ret && !wantStop_.load()) {
          uint16_t ret_ = feed_->feedData(buf + written, ret - written, tid, false);
          if (ret_ == 0) BELL_SLEEP_MS(10);
          written += ret_;
        }
      }

      if (onState_) onState_(false);

      if (wantStop_.load()) {
        // final cleanup
        if (feed_) {
          feed_->feedCommand(AudioControl::FLUSH, 0);
          feed_->feedCommand(AudioControl::DISC, 0);
        }
        wantStop_.store(false);
        isRunning_.store(false);
        break;
      }
      else {
        if (feed_) feed_->feedCommand(AudioControl::SKIP, 0);
        sleepMs(reconnectDelayMs_);
        wantRestart_.store(ret < 0); // reconnect only on unexpected end
      }
    }
  }

  // -------------- shared helpers --------------
  void emitMeta(const std::string& station, const std::string& title) {
    if (onMeta_) onMeta_(station, title);
  }

  void reportError(const std::string& msg) {
    SC32_LOG(error, "%s", msg.c_str());
    if (onError_) onError_(msg);
  }

  static void sleepMs(uint32_t ms) {
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(ms));
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
  }
public:
  // utils
  static std::string toLower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
  static bool startsWith(const std::string& s, const char* pfx) { return s.rfind(pfx, 0) == 0; }
  static bool endsWith(const std::string& s, const char* sfx) { size_t n = strlen(sfx); return s.size() >= n && std::equal(s.end() - n, s.end(), sfx); }
  static void ltrim(std::string& s) { s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {return !std::isspace(ch);})); }
  static void rtrim(std::string& s) { s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {return !std::isspace(ch);}).base(), s.end()); }
  static void trim(std::string& s) { ltrim(s); rtrim(s); }
  static int  toInt(std::string_view sv) { int v = 0; for (char c : sv) if (std::isdigit((unsigned char)c)) v = v * 10 + (c - '0'); return v; }
  static uint32_t parseUint(const std::string& s) { uint32_t v = 0; for (char c : s) { if (c < '0' || c>'9') break; v = v * 10 + (uint32_t)(c - '0'); if (v > 36000u) break; } return v; }
  static std::string svToString(std::string_view sv) { return std::string(sv.data(), sv.size()); }

  std::shared_ptr<AudioControl>                audio_;
  std::shared_ptr<AudioControl::FeedControl> feed_;
protected:

  std::atomic<bool> isRunning_{ false };
  std::atomic<bool> wantStop_{ false };
  std::atomic<bool> wantRestart_{ false };

  std::mutex   mu_;
  std::string  targetUri_;
  std::string  displayName_;
  uint32_t     trackId_ = 0;

  MetaCb  onMeta_;
  ErrorCb onError_;
  StateCb onState_;

  const uint32_t reconnectDelayMs_ = 1500;
  static constexpr const char* ua_ = "StreamCore32/StreamBase (ESP32)";
};
