// ============================
// include/streamcore/WebStream.h
// ============================
#pragma once
#include <algorithm>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "AudioControl.h"
#include "BellTask.h"
#include "HTTPClient.h"
#include "Logger.h"
#include "MetaPoller.h"  // your existing poller helper

class WebStream : public StreamBase {
 public:
  using MetaCb = std::function<void(const std::string&, const std::string&)>;
  using ErrorCb = std::function<void(const std::string&)>;
  using StateCb = std::function<void(bool)>;

  struct MetaSpec {
    MetaPoller::Kind kind = MetaPoller::Kind::Auto;
    std::string url;             // optional explicit endpoint (abs or relative)
    uint32_t intervalMs = 5000;  // polling cadence
    bool enabled = true;
    bool fallbackOnEmptyICY = true;  // keep poller alive if ICY is empty
    bool autoDisarmOnICY = true;     // disarm poller once non-empty ICY seen
  };

  WebStream(std::shared_ptr<AudioControl> audio)
      : StreamBase("WebStream", audio, 1024 * 16, 1, 1, false) {
    // metadata poller (idles until armed)
    poller_ = std::make_shared<MetaPoller>(
        [this](const std::string& s, const std::string& t) {
          if (onMeta_)
            onMeta_(s, t);
        },
        [this](const std::string& m) {
          if (onError_)
            onError_(m);
        });
    poller_->startTask();
  }

  ~WebStream() override {
    if (poller_) {
      poller_->stopTask();
      while (poller_->isRunning())
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    if (isRunning_.load())
      stop();
  }

  // ---- callbacks ----
  void onMetadata(MetaCb cb) { onMeta_ = std::move(cb); }
  void onError(ErrorCb cb) { onError_ = std::move(cb); }
  void onState(StateCb cb) { onState_ = std::move(cb); }

  // ---- config ----
  void setMetaSpec(const MetaSpec& s) {
    std::lock_guard<std::mutex> lk(mu_);
    metaSpec_ = s;
  }

  // ---- control ----
  // Can be called while playing — causes a seamless restart to new URI
  void play(const std::string& uri,
            const std::string& displayName = std::string()) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      targetUri_ = uri;
      displayName_ = displayName;
    }
    wantRestart_.store(true);
    if (!isRunning_.load())
      startTask();
  }

  void stop() {
    wantStop_.store(true);
    wantRestart_.store(false);
    if (poller_)
      poller_->disarm();
    std::scoped_lock<std::mutex> lk(isRunningMutex_);
  }
  struct IcyHeaders {
    int metaInt = 0;
    std::string contentType;
    std::string stationName;
    std::string codec;
    uint32_t bitrateKbps = 0;
    uint32_t sampleRateHz = 0;
    uint8_t channels = 0;
  };
  IcyHeaders& getIcyHeaders() { return H; }
  uint8_t state = 0;

 protected:
  void runTask() override {
    std::scoped_lock<std::mutex> lk(isRunningMutex_);
    isRunning_.store(true);
    feed_->state_callback = [this](uint8_t s) {
      state = s;
    };
    while (isRunning_.load()) {
      if (!wantRestart_.load()) {
        vTaskDelay(pdMS_TO_TICKS(25));
        continue;
      }

      std::string name;
      {
        std::lock_guard<std::mutex> lk(mu_);
        resolvedUri_ = targetUri_;
        name = displayName_;
      }
      wantRestart_.store(false);
      if (resolvedUri_.empty()) {
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }

      // Resolve possible playlists
      auto resolved = resolveIfPlaylist(resolvedUri_);
      if (!resolved) {
        reportError("resolve failed");
        vTaskDelay(pdMS_TO_TICKS(1000));
        wantRestart_.store(true);
        continue;
      } else
        SC32_LOG(info, "Resolved to %s", (*resolved).c_str());
      wantRestart_.store(true);
      // bump track id so the sink treats it as new
      const uint32_t tid = audio_->makeUniqueTrackId();
      if (onState_)
        onState_(true);
      auto resp = open(*resolved, name, tid);
      if (resp == nullptr)
        continue;
      wantStop_.store(false);
      const size_t CHUNK = 1024;
      uint8_t buf[CHUNK];
      int ret = 0;
      while (!wantStop_.load()) {
        ret = read(resp.get(), buf, CHUNK, tid);
        if (ret < 0)
          break;
        if (ret == 0) {
          BELL_SLEEP_MS(10);
          wantRestart_.store(true);
          wantStop_.store(true);
          break;
        }
        uint16_t written = 0;
        while (written < ret && !wantStop_.load()) {
          uint16_t ret_ =
              feed_->feedData(buf + written, ret - written, tid, false);
          if (ret_ == 0)
            BELL_SLEEP_MS(10);
          written += ret_;
        }
      }

      //const bool cleanEnd = streamOnce(*resolvedUri_, name, tid);
      if (onState_)
        onState_(false);

      if (wantStop_.load() && !wantRestart_.load()) {
        if (feed_) {
          feed_->feedCommand(AudioControl::FLUSH, 0);
          feed_->feedCommand(AudioControl::DISC, 0);
        }
        if (poller_)
          poller_->disarm();
        wantStop_.store(false);
        isRunning_.store(false);
      } else {
        if (feed_)
          feed_->feedCommand(AudioControl::SKIP, 0);
        if (poller_)
          poller_->disarm();
        vTaskDelay(pdMS_TO_TICKS(reconnectDelayMs_));
        // reconnect if unexpected end or user queued another play
        wantRestart_.store(true);
      }
    }
    while (state != 7)
      BELL_SLEEP_MS(10);
    isRunning_.store(false);
  }

 private:
  std::unique_ptr<bell::HTTPClient::Response> open(const std::string& url,
                                                   const std::string& station,
                                                   uint32_t trackId) {

    bell::HTTPClient::Headers hdrs = {{"Icy-MetaData", "1"},
                                      {"User-Agent", ua_}};
    auto resp = bell::HTTPClient::get(url, hdrs, 32);
    if (!resp) {
      reportError("HTTP connect failed");
      return nullptr;
    }
    isChunked_ = false;
    auto h = resp->headers();
    for (auto& header : h) {
      std::string lh = toLower(header.first);
      if (lh == "content-type") {
        std::string lc = toLower(header.second);
        if (lc.find("audio/mpeg") != std::string::npos ||
            lc.find("audio/mp3") != std::string::npos ||
            lc.find("audio/x-mpeg") != std::string::npos) {
          H.codec = "Mp3";
        } else if (lc.find("audio/aac") != std::string::npos ||
                   lc.find("aacp") != std::string::npos ||
                   lc.find("audio/aacp") != std::string::npos ||
                   lc.find("audio/mp4") != std::string::npos ||
                   lc.find("application/aac") != std::string::npos) {
          H.codec = "AAC";
          BELL_LOG(debug, "webstream", "AAC");
        } else if (lc.find("audio/ogg") != std::string::npos ||
                   lc.find("application/ogg") != std::string::npos) {
          if (lc.find("opus") != std::string::npos)
            H.codec = "Opus";
          else if (lc.find("vorbis") != std::string::npos)
            H.codec = "Vorbis";
          else
            H.codec = "Ogg";
        } else if (lc.find("audio/wav") != std::string::npos ||
                   lc.find("audio/x-wav") != std::string::npos ||
                   lc.find("audio/l16") != std::string::npos) {
          H.codec = "Pcm";
        } else if (lc.find("audio/flac") != std::string::npos ||
                   lc.find("flac") != std::string::npos) {
          H.codec = "FLAC";
        } else
          H.codec = "unknown";
      } else if ((lh == "icy-name" || lh == "name") && !header.second.empty()) {
        H.stationName = header.second;
      } else if ((lh == "icy-br" || lh == "icy-bitrate" || lh == "br") &&
                 !header.second.empty()) {
        H.bitrateKbps = toInt(header.second);
        if (H.bitrateKbps > 320 && H.bitrateKbps < 2000000)
          H.bitrateKbps /= 1000;
      } else if ((lh == "icy-sr" || lh == "samplerate" || lh == "sr") &&
                 !header.second.empty()) {
        H.sampleRateHz = toInt(header.second);
      } else if ((lh == "icy-channels" || lh == "channels" || lh == "ch") &&
                 !header.second.empty()) {
        H.channels = static_cast<uint8_t>(toInt(header.second));
      } else if (lh == "icy-metaint") {
        H.metaInt = toInt(header.second);
      } else if (lh == "transfer-encoding" &&
                 header.second.find("chunked") != std::string::npos) {
        isChunked_ = true;
      }
    }
    if (H.stationName.empty())
      H.stationName = station;
    SC32_LOG(info, "headers: %s %d %s", H.contentType.c_str(), H.metaInt,
             H.stationName.c_str());

    hadNonEmptyICY_ = false;
    if (poller_ && metaSpec_.enabled &&
        metaSpec_.kind != MetaPoller::Kind::Disabled) {
      MetaPoller::Spec ps;
      ps.kind = (MetaPoller::Kind)metaSpec_.kind;
      ps.url = metaSpec_.url;
      ps.intervalMs = metaSpec_.intervalMs;
      ps.enabled = metaSpec_.enabled;
      if (H.metaInt <= 0)
        poller_->arm(originFromUrl(url), H.stationName, ps);
      else if (metaSpec_.fallbackOnEmptyICY && H.stationName.empty())
        poller_->arm(originFromUrl(url), H.stationName, ps);
      else
        poller_->disarm();
    }
    bytesUntilMeta =
        (H.metaInt > 0) ? H.metaInt : std::numeric_limits<int>::max();
    return resp;
  }
  int read(bell::HTTPClient::Response* stream, uint8_t* buffer,
           size_t chunk_size, size_t trackId) {

    size_t want = (size_t)std::min<int>(bytesUntilMeta, (int)chunk_size);
    int got = 0;
    if (!isChunked_) {
      got = stream->read(buffer, want);
    } else {
      got = readChunkedBody(stream, buffer, want);
    }
    if (got <= 0)
      return got;
    bytesUntilMeta -= (int)got;
    if (bytesUntilMeta == 0) {

      uint8_t Lbyte = 0;
      int r;
      if (!isChunked_) {
        if (stream->read(&Lbyte, 1) != 1)
          return -1;
      } else {
        r = readChunkedBody(stream, &Lbyte, 1);
        if (r <= 0)
          return r;
      }
      int metaLen = (int)Lbyte * 16;
      if (metaLen > 0) {
        std::string meta;
        meta.resize((size_t)metaLen);
        if (!isChunked_) {
          stream->readExact(reinterpret_cast<uint8_t*>(&meta[0]), metaLen);
          // readExact already handles short reads
        } else {
          int m = readChunkedBody(stream, reinterpret_cast<uint8_t*>(&meta[0]),
                                  (size_t)metaLen);
          if (m != metaLen) {
            // truncated metadata; bail out
            return -1;
          }
        }
        parseAndEmitIcy(meta, H.stationName);
      } else if (poller_ && metaSpec_.enabled &&
                 metaSpec_.kind != MetaPoller::Kind::Disabled &&
                 metaSpec_.fallbackOnEmptyICY && !hadNonEmptyICY_) {
        MetaPoller::Spec ps;
        ps.kind = MetaPoller::Kind::Auto;
        ps.url = metaSpec_.url;
        ps.intervalMs = metaSpec_.intervalMs;
        ps.enabled = metaSpec_.enabled;
        poller_->arm(originFromUrl(resolvedUri_), H.stationName, ps);
      }
      bytesUntilMeta =
          (H.metaInt > 0) ? H.metaInt : std::numeric_limits<int>::max();
    }
    return got;
  }
  // ---------- playlist helpers ----------
  static bool hasPlaylistExt(const std::string& u) {
    auto L = toLower(u);
    return endsWith(L, ".m3u") || endsWith(L, ".m3u8") || endsWith(L, ".pls");
  }

  std::optional<std::string> resolveIfPlaylist(const std::string& url) {
    if (hasPlaylistExt(url))
      return fetchPlaylist(url);
    bell::HTTPClient::Headers hdrs = {{"Icy-MetaData", "1"},
                                      {"User-Agent", ua_}};
    auto resp = bell::HTTPClient::get(url, hdrs, 32);
    if (!resp)
      return std::nullopt;
    auto ctype = svToString(resp->header("content-type"));
    if (startsWith(toLower(ctype), "audio/"))
      return url;
    if (isPlaylistContentType(ctype)) {
      auto body_sv = resp->body();
      std::string body(body_sv.data(), body_sv.size());
      return parsePlaylistBody(body);
    }
    return url;
  }

  std::optional<std::string> fetchPlaylist(const std::string& url) {
    auto resp = bell::HTTPClient::get(url, {{"User-Agent", ua_}}, 32);
    if (!resp)
      return std::nullopt;
    auto body_sv = resp->body();
    std::string body(body_sv.data(), body_sv.size());
    return parsePlaylistBody(body);
  }

  static bool isPlaylistContentType(const std::string& ct) {
    auto L = toLower(ct);
    return startsWith(L, "audio/x-mpegurl") ||
           startsWith(L, "application/vnd.apple.mpegurl") ||
           startsWith(L, "application/x-mpegURL") ||
           startsWith(L, "application/pls") || startsWith(L, "audio/x-scpls") ||
           startsWith(L, "text/");
  }

  static std::optional<std::string> parsePlaylistBody(const std::string& body) {
    size_t pos = 0;
    bool first = true;
    while (pos < body.size()) {
      size_t end = body.find_first_of("\r\n", pos);
      size_t len =
          (end == std::string::npos) ? (body.size() - pos) : (end - pos);
      std::string line = body.substr(pos, len);
      if (end == std::string::npos)
        pos = body.size();
      else
        pos = (body[end] == '\r' && end + 1 < body.size() &&
               body[end + 1] == '\n')
                  ? end + 2
                  : end + 1;
      if (first && line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
          (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
        line.erase(0, 3);
      first = false;
      trim(line);
      if (line.empty())
        continue;
      if (line[0] == '#' || line[0] == ';' || line[0] == '[')
        continue;
      auto eq = line.find('=');
      std::string cand = (eq != std::string::npos) ? line.substr(eq + 1) : line;
      trim(cand);
      if (startsWith(cand, "http://") || startsWith(cand, "https://"))
        return cand;
    }
    return std::nullopt;
  }

  // ---------- streaming ----------

  bool streamOnce(const std::string& url, const std::string& station,
                  uint32_t trackId) {
    bell::HTTPClient::Headers hdrs = {{"Icy-MetaData", "1"},
                                      {"User-Agent", ua_}};
    auto resp = bell::HTTPClient::get(url, hdrs, 32);
    if (!resp) {
      reportError("HTTP connect failed");
      return false;
    }

    IcyHeaders H;
    auto h = resp->headers();
    for (auto& header : h) {
      BELL_LOG(debug, "webstream", "Checking header: %s: %s",
               header.first.c_str(), header.second.c_str());
    }
    H.metaInt = toInt(resp->header("icy-metaint"));
    std::string meta = svToString(resp->header("icy-audio-info"));
    std::string br = svToString(resp->header("icy-br"));

    H.contentType = svToString(resp->header("content-type"));
    H.stationName = svToString(resp->header("icy-name"));
    if (H.stationName.empty())
      H.stationName = station;
    SC32_LOG(info, "headers: %s %d %s", H.contentType.c_str(), H.metaInt,
             H.stationName.c_str());

    hadNonEmptyICY_ = false;
    if (poller_ && metaSpec_.enabled &&
        metaSpec_.kind != MetaPoller::Kind::Disabled) {
      MetaPoller::Spec ps;
      ps.kind = (MetaPoller::Kind)metaSpec_.kind;
      ps.url = metaSpec_.url;
      ps.intervalMs = metaSpec_.intervalMs;
      ps.enabled = metaSpec_.enabled;
      if (H.metaInt <= 0)
        poller_->arm(originFromUrl(url), H.stationName, ps);
      else if (metaSpec_.fallbackOnEmptyICY && H.stationName.empty())
        poller_->arm(originFromUrl(url), H.stationName, ps);
      else
        poller_->disarm();
    }

    auto& is = resp->stream();
    const size_t CHUNK = 1024;
    std::vector<uint8_t> buf(CHUNK);
    int bytesUntilMeta =
        (H.metaInt > 0) ? H.metaInt : std::numeric_limits<int>::max();
    isRunning_.store(true);
    while (isRunning_.load()) {
      size_t want = (size_t)std::min<int>(bytesUntilMeta, (int)CHUNK);
      auto got = resp->read(buf.data(), want);
      if (got <= 0)
        break;
      size_t fed = feed_->feedData(buf.data(), (size_t)got, trackId, false);
      if (fed == 0)
        vTaskDelay(pdMS_TO_TICKS(1));
      bytesUntilMeta -= (int)got;
      if (bytesUntilMeta == 0) {
        uint8_t L = 0;
        if (resp->read(&L, 1) != 1)
          break;
        int metaLen = L * 16;
        if (metaLen > 0) {
          std::string meta;
          meta.resize((size_t)metaLen);
          resp->readExact(reinterpret_cast<uint8_t*>(&meta[0]), metaLen);
          parseAndEmitIcy(meta, H.stationName);
        } else if (poller_ && metaSpec_.enabled &&
                   metaSpec_.kind != MetaPoller::Kind::Disabled &&
                   metaSpec_.fallbackOnEmptyICY && !hadNonEmptyICY_) {
          MetaPoller::Spec ps;
          ps.kind = MetaPoller::Kind::Auto;
          ps.url = metaSpec_.url;
          ps.intervalMs = metaSpec_.intervalMs;
          ps.enabled = metaSpec_.enabled;
          poller_->arm(originFromUrl(url), H.stationName, ps);
        }
        bytesUntilMeta =
            (H.metaInt > 0) ? H.metaInt : std::numeric_limits<int>::max();
      }
      vTaskDelay(pdMS_TO_TICKS(1));
    }

    return true;  // true if asked to stop; false = unexpected end
  }

  // ---------- ICY helpers ----------
  static std::string parseStreamTitle(const std::string& meta) {
    std::string m = meta;
    // remove embedded NULs
    m.erase(std::remove(m.begin(), m.end(), '\0'), m.end());

    std::string ml = toLower(m);
    const std::string key = "streamtitle=";
    size_t p = ml.find(key);
    if (p == std::string::npos)
      return {};

    size_t v = p + key.size();
    if (v >= m.size())
      return {};

    // skip whitespace after '='
    while (v < m.size() && (m[v] == ' ' || m[v] == '\t'))
      ++v;
    if (v >= m.size())
      return {};

    // find end of value at first ';' (or end of string)
    size_t end = m.find(';', v);
    if (end == std::string::npos)
      end = m.size();

    size_t s = v;
    size_t e = end;

    // strip *outer* quotes only, keep inner quotes like in DESTINY'S
    if (s < e && (m[s] == '\'' || m[s] == '"')) {
      char quote = m[s];
      ++s;
      if (e > s && m[e - 1] == quote) {
        --e;
      }
    }

    std::string val = m.substr(s, e - s);
    trim(val);
    return val;
  }
  void parseAndEmitIcy(const std::string& raw, const std::string& station) {
    auto title = parseStreamTitle(raw);
    if (!title.empty()) {
      hadNonEmptyICY_ = true;
      if (onMeta_)
        onMeta_(station, title);
      if (poller_ && metaSpec_.autoDisarmOnICY)
        poller_->disarm();
    }
  }
  static bool readLine(HTTPClient::Response* is, std::string& out) {
    out.clear();
    char c;
    while (true) {
      if (!is->read(reinterpret_cast<uint8_t*>(&c), 1))
        return false;
      if (c == '\r') {
        // expect \n
        if (!is->read(reinterpret_cast<uint8_t*>(&c), 1))
          return false;
        return true;
      }
      if (c == '\n')
        return true;
      out.push_back(c);
    }
  }

  static size_t parseHexSize(const std::string& s) {
    size_t val = 0;
    for (char c : s) {
      if (c == ';' || c == ' ' || c == '\t')
        break;  // ignore extensions
      int v = -1;
      if (c >= '0' && c <= '9')
        v = c - '0';
      else if (c >= 'a' && c <= 'f')
        v = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
        v = c - 'A' + 10;
      else
        break;
      val = (val << 4) | (size_t)v;
    }
    return val;
  }
  int readChunkedBody(HTTPClient::Response* stream, uint8_t* dst, size_t max) {
    size_t out = 0;

    while (out < max) {
      // Need a new chunk?
      if (chunkBytesRemaining_ == 0) {
        std::string line;
        if (!readLine(stream, line)) {
          return (out > 0) ? (int)out : -1;  // EOF/error
        }

        size_t sz = parseHexSize(line);
        if (sz == 0) {
          // terminal chunk: spec says trailers follow, but for streaming we just stop
          return (int)out;
        }
        chunkBytesRemaining_ = sz;
      }

      size_t want = std::min(chunkBytesRemaining_, max - out);
      auto got = stream->read(dst + out, want);
      if (got <= 0) {
        return (out > 0) ? (int)out : (int)got;
      }

      chunkBytesRemaining_ -= (size_t)got;
      out += (size_t)got;

      if (chunkBytesRemaining_ == 0) {
        // Consume CRLF after the chunk body
        char crlf[2];
        stream->read(reinterpret_cast<uint8_t*>(crlf), 2);
        // don’t care if it's exactly "\r\n", we’re just best effort
      }
    }

    return (int)out;
  }
  // ---------- tiny utils ----------
  static std::string toLower(std::string s) {
    for (char& c : s)
      c = (char)std::tolower((unsigned char)c);
    return s;
  }
  static bool startsWith(const std::string& s, const char* pfx) {
    return s.rfind(pfx, 0) == 0;
  }
  static bool endsWith(const std::string& s, const char* sfx) {
    size_t n = strlen(sfx);
    return s.size() >= n && std::equal(s.end() - n, s.end(), sfx);
  }
  static void ltrim(std::string& s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
              return !std::isspace(ch);
            }));
  }
  static void rtrim(std::string& s) {
    s.erase(std::find_if(s.rbegin(), s.rend(),
                         [](unsigned char ch) { return !std::isspace(ch); })
                .base(),
            s.end());
  }
  static void trim(std::string& s) {
    ltrim(s);
    rtrim(s);
  }
  static int toInt(std::string_view sv) {
    int v = 0;
    for (char c : sv)
      if (std::isdigit((unsigned char)c))
        v = v * 10 + (c - '0');
    return v;
  }
  static std::string svToString(std::string_view sv) {
    return std::string(sv.data(), sv.size());
  }
  static std::string originFromUrl(const std::string& url) {
    auto p = url.find("://");
    if (p == std::string::npos)
      return url;
    auto start = p + 3;
    auto slash = url.find('/', start);
    if (slash == std::string::npos)
      return url.substr(0, url.size());
    return url.substr(0, slash);
  }

 private:
  std::shared_ptr<MetaPoller> poller_;

  // state
  std::atomic<bool> isRunning_{false};
  std::atomic<bool> wantStop_{false};
  std::atomic<bool> wantRestart_{false};
  std::mutex isRunningMutex_;
  bool isChunked_ = false;
  size_t chunkBytesRemaining_ = 0;
  std::mutex mu_;
  std::string targetUri_;
  std::string resolvedUri_;
  std::string displayName_;
  uint32_t trackId_ = 0;

  MetaSpec metaSpec_{};
  IcyHeaders H{};
  bool hadNonEmptyICY_ = false;
  int bytesUntilMeta = std::numeric_limits<int>::max();
  // callbacks
  MetaCb onMeta_;
  ErrorCb onError_;
  StateCb onState_;

  const uint32_t reconnectDelayMs_ = 1500;
  static constexpr const char* ua_ = "StreamCore32/WebStream (ESP32)";
};
