#include "QobuzPlayer.h"

constexpr size_t BUF_CAP = 4 * 1024;
constexpr size_t PULL_BYTES = 4 * 1024;
constexpr size_t HEADROOM = 1 * 1024;
constexpr size_t PROBE_MAX = 1 * 1024;

// --- local predefs ---
// --- event helpers ---
static inline std::string build_start_event(std::shared_ptr<qobuz::QobuzQueueTrack> track,
  const std::string& user_id, int played_for_s);
static inline std::string build_end_event(std::shared_ptr<qobuz::QobuzQueueTrack> track,
  const std::string& user_id, int played_for_s);
// --- HTTP helpers ---
static inline bool check_http_status(const uint16_t status);
static inline size_t parse_content_range_total(const std::string& cr);
static inline std::unique_ptr<bell::HTTPClient::Response> open_at(const std::string& url, std::optional<size_t> pos = std::nullopt, bool keepAlive = false);
// --- AudioFile helpers ---
static inline size_t ms_to_offset(size_t fileSize, size_t duration_ms, size_t pos_ms);
// --- FLAC helpers ---
static inline void create_flac_metadata(
  uint8_t* dst,
  uint32_t sample_rate,
  uint8_t  channels,      // 1..8
  uint8_t  bits_per_sample, // 4..32
  size_t total_samples, // 0 = unknown
  uint16_t block_size = 4096);
static bool parse_flac_frame_header(bell::HTTPClient::Response* s,
  const uint8_t* buf, size_t have_after_sync,
  uint32_t& sr, uint8_t& ch, uint8_t& bps, uint16_t& bs);
static inline size_t fetch_flac_metadata(bell::HTTPClient::Response* s, uint8_t* dst, size_t n);
static inline bool sync_ff_f8fb(const uint8_t* p, size_t n);
//
//---QobuzPlayer---
QobuzPlayer::QobuzPlayer(std::shared_ptr<AudioControl> audio, std::shared_ptr<qobuz::QobuzQueue> queue)
  : StreamBase("Qobuz_Player", audio, 1024 * 12, 4, 1, 1) {
  queue_ = queue;
  feed_->state_callback = [this](uint8_t st) {
    if (st == 1) {
      current_track_playing = current_track_buffering;
      player_state.playingState = qconnect_PlayingState_PLAYING_STATE_PLAYING;
      player_state.currentPosition.timestamp = timesync::now_ms();
      current_track_playing->startedPlayingAt = player_state.currentPosition.timestamp;
      auto resp = on_qobuz_post_("track", "reportStreamingStart",
        build_start_event(current_track_playing, user_id_, 0),
        {}, false);
      if (!hb_) {
        hb_ = std::make_unique<Heartbeat>([&]() {
          sendPlayerState();
          });
        hb_->start();
      }
      sendPlayerState();
      if (hb_) hb_->delay();
      //if(resp->status() != 200) SC32_LOG(info, "reportStreamingStart %s", resp->body_string().c_str());
    }
    else if (st == 3) {
      player_state.playingState = qconnect_PlayingState_PLAYING_STATE_PAUSED;
      player_state.currentPosition.value = timesync::now_ms() - player_state.currentPosition.timestamp + player_state.currentPosition.value;
      auto resp = on_qobuz_post_("track", "reportStreamingEndJson",
        build_end_event(current_track_playing, user_id_, (timesync::now_ms() - current_track_playing->startedPlayingAt) / 1000),
        {}, false);
      sendPlayerState();
      if (hb_) hb_->delay();
    }
    else if (st == 7) {
      auto resp = on_qobuz_post_("track", "reportStreamingEndJson", build_end_event(current_track_playing, user_id_, (timesync::now_ms() - current_track_playing->startedPlayingAt) / 1000), {}, false);
      if (resp->status() != 200) SC32_LOG(info, "reportStreamingEnd %s", resp->body_string().c_str());
      SC32_LOG(info, "QobuzPlayer: track ended");
      if (hb_) hb_.reset();
      SC32_LOG(info, "QobuzPlayer: heartbeat stopped");
    }
    if (onState_) onState_(st != 3 && st != 7);
    if (onUiMessage_ && (st == 2 || st == 3)) {
      uint64_t pos_ms = 0;
      if (st == 2) {
        pos_ms = timesync::now_ms() - player_state.currentPosition.timestamp + player_state.currentPosition.value;
      }
      else {
        pos_ms = player_state.currentPosition.value;
      }
      nlohmann::json j;
      j["type"] = "playback";
      j["src"] = "Qobuz";
      std::string s;
      uint32_t sr;
      switch (current_track_playing->format) {
      case qobuz::AudioFormat::QOBUZ_QUEUE_FORMAT_MP3:
        j["quality"] = "MP3 - 320kbps";
        break;
      case qobuz::AudioFormat::QOBUZ_QUEUE_FORMAT_FLAC_LOSSLESS:
        [[fallthrough]];
      case qobuz::AudioFormat::QOBUZ_QUEUE_FORMAT_FLAC_HI_RES_96:
        [[fallthrough]];
      case qobuz::AudioFormat::QOBUZ_QUEUE_FORMAT_FLAC_HI_RES_192:
        s = "FLAC - ";
        s += std::to_string(current_track_playing->bits_depth) + "-Bit / ";
        sr = current_track_playing->sampling_rate / 1000;
        s += std::to_string(sr);
        sr *= 1000;
        if (current_track_playing->sampling_rate - sr > 0) s += "." + std::to_string((current_track_playing->sampling_rate - sr) / 100);
        s += "kHz";
        j["quality"] = s;
        break;
      default:
        j["quality"] = "Unknown";
        break;
      }
      j["state"] = (int)(st == 2);
      j["position_ms"] = pos_ms;
      j["duration_ms"] = current_track_playing->durationMs;
      j["track"] = {
        {"title", current_track_playing->title},
        {"artist", current_track_playing->artist.name},
        {"album", current_track_playing->album.name},
        {"image", current_track_playing->album.image.large_img}
      };
      onUiMessage_(j.dump());
    }
    };
  SC32_LOG(info, "QobuzPlayer created");
}

bool QobuzPlayer::getStreamInfo(std::string url, size_t& length, size_t& offset, qobuz::AudioFormat format, uint8_t* buffer) {
  auto resp = open_at(url);
  if (!resp || !resp->stream().isOpen()) return false;
  length = 0;
  offset = 0;
  while (true) {
    if (!check_http_status(resp->status())) break;
    auto ctype = svToString(resp->header("content-type"));
    if (ctype.empty()) break;
    if (ctype.rfind("audio/", 0) != 0 && ctype != "application/octet-stream") break;
    auto crange = svToString(resp->header("content-range"));
    length = resp->totalLength();
    if (format >= qobuz::AudioFormat::QOBUZ_QUEUE_FORMAT_FLAC_LOSSLESS) {
      if (!probeFlac(resp.get(), length, buffer, offset)) break;
    }
    SC32_LOG(info, "getStreamInfo length: %lu offset: %lu", length, offset);
    if (resp->stream().isOpen()) {
      resp->drainBody();
      resp->stream().close();
    }
    return true;
  }
  if (resp->stream().isOpen()) {
    resp->drainBody();
    resp->stream().close();
  }
  return false;
}
void QobuzPlayer::runTask() {
  std::scoped_lock lock(isRunningMutex_);
  constexpr size_t BUF_CAP = 32 * 1024;
  constexpr size_t PULL_BYTES = 4 * 1024;
  constexpr size_t HEADROOM = 1 * 1024;
  constexpr size_t PROBE_MAX = 1 * 1024;

  int retries = 0;
  bool initial_seek = false;
  isRunning_.store(true);
  wantRestart_.store(false);
  size_t tid = 0;
  player_state = qconnect_QueueRendererState{
    .has_playingState = true,
    .playingState = qconnect_PlayingState_PLAYING_STATE_PLAYING,
    .has_bufferState = true,
    .bufferState = qconnect_BufferState_BUFFER_STATE_OK,
    .has_currentPosition = true,
    .currentPosition = {
      .has_timestamp = true,
      .timestamp = timesync::now_ms(),
      .has_value = true,
      .value = 0,
    },
    .has_duration = true,
    .duration = 0,
    .has_queueVersion = true,
    .queueVersion = queue_->queueuState.queueVersion,
    .has_currentQueueItemId = true,
    .currentQueueItemId = 0,
    .has_nextQueueItemId = false,
    .nextQueueItemId = 0,
  };
  current_track_buffering = nullptr;
  while (isRunning_.load()) {
    std::unique_ptr<bell::HTTPClient::Response> resp = nullptr;
    if (wantRestart_.load() && retries > 0) {
      BELL_SLEEP_MS(50);
      if (retries == 1) current_track_buffering->format = qobuz::AudioFormat::QOBUZ_QUEUE_FORMAT_MP3;
      else if (current_track_buffering->format > qobuz::AudioFormat::QOBUZ_QUEUE_FORMAT_FLAC_LOSSLESS) current_track_buffering->format = qobuz::AudioFormat::QOBUZ_QUEUE_FORMAT_FLAC_LOSSLESS;
      queue_->getFileUrl(current_track_buffering);
      SC32_LOG(info, "restarting (retries %d)", retries);
      BELL_YIELD();
    }
    else {
      current_track_buffering = queue_->consumeTrack(current_track_buffering, player_state.nextQueueItemId);
      if (!current_track_buffering) break;
      if (current_track_buffering->state == qobuz::QueuedTrackState::FAILED) continue;
      if (player_state.nextQueueItemId == 0) {
        player_state.has_nextQueueItemId = false;
      }
      else player_state.has_nextQueueItemId = true;
      player_state.currentQueueItemId = current_track_buffering->index;
      player_state.playingState = qconnect_PlayingState_PLAYING_STATE_UNKNOWN;
      player_state.bufferState = qconnect_BufferState_BUFFER_STATE_BUFFERING;
      player_state.duration = current_track_buffering->durationMs;
      player_state.currentPosition.value = 0;
      player_state.currentPosition.timestamp = 0;
      player_state.queueVersion = queue_->queueuState.queueVersion;

      current_track_buffering->wantSkip_.store(true);
      current_track_buffering->skipTo_.store(current_track_buffering->startMs);
      current_track_buffering->startMs = 0;

      tid = audio_->makeUniqueTrackId();
      retries = 3;
      wantRestart_.store(true);
      initial_seek = true;
    }
    std::string url = current_track_buffering->fileUrl;
    if (url.empty()) { SC32_LOG(error, "empty URL"); isRunning_.store(false); return; }

    auto buf = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[BUF_CAP]);
    if (!buf) { SC32_LOG(error, "OOM"); isRunning_.store(false); return; }

    size_t in_len = 0, out_pos = 0;
    size_t n = 0;
    bool abortTrack = false;
    baseOffset_ = 0;

    uint8_t flac_header[42];
    if (!getStreamInfo(url, totalSize_, baseOffset_, current_track_buffering->format, flac_header)) {
      SC32_LOG(error, "failed to get track info");
      BELL_YIELD();
      retries--;
      continue;
    }
    else if (current_track_buffering->format >= qobuz::AudioFormat::QOBUZ_QUEUE_FORMAT_FLAC_LOSSLESS) {
      BELL_YIELD();
      feed_->feedData(flac_header, 42, tid);
      totalSize_ -= baseOffset_;
    }

    size_t respRemaining = std::numeric_limits<size_t>::max();
    player_state.bufferState = qconnect_BufferState_BUFFER_STATE_OK;
    // --------------- main loop ---------------
    while (!wantStop_.load()) {
      // External seek
      if (current_track_buffering->wantSkip_.load()) {
        size_t newN = current_track_buffering->skipTo_.load();
        SC32_LOG(info, "seek to %lu", newN);
        player_state.currentPosition.value = newN;
        player_state.currentPosition.timestamp = timesync::now_ms();
        if (newN) {
          double p01 = 1.0 * newN / (double)current_track_buffering->durationMs;
          newN = (size_t)(p01 * (double)(totalSize_));
        }
        if (resp && resp->stream().isOpen()) {
          resp->drainBody();
          resp->stream().close();
        }
        resp = open_at(url, newN + baseOffset_);
        if (!resp || !resp->stream().isOpen()) {
          SC32_LOG(error, "resume at %lu failed", newN);
          BELL_YIELD();
          current_track_buffering->wantSkip_.store(false);
          abortTrack = true; break;
        }
        int st2 = resp->status();
        if (st2 != 206 && st2 != 200) {
          if (st2 == 416) { eofSeen_.store(true); }
          else SC32_LOG(error, "resume HTTP %d", st2);
          BELL_YIELD();
          current_track_buffering->wantSkip_.store(false);
          abortTrack = true; break;
        }
        n = newN;
        respRemaining = totalSize_ - n;
        BELL_YIELD();
        if (respRemaining <= 0) respRemaining = std::numeric_limits<size_t>::max();
        current_track_buffering->wantSkip_.store(false);
        if (!initial_seek) {
          on_qobuz_post_("track", "reportStreamingEndJson", build_end_event(
            current_track_buffering, user_id_,
            (timesync::now_ms() - current_track_buffering->startedPlayingAt) / 1000
          ),
            {},
            false);
          sendPlayerState();
          hb_->delay();
        } initial_seek = false;
      }
      if (abortTrack) break;
      // FEED buffered
      if (out_pos < in_len) {
        size_t fed = feed_->feedData(buf.get() + out_pos, (size_t)(in_len - out_pos), tid);
        if (fed > 0) { out_pos += (size_t)fed; n += (size_t)fed; }
        else BELL_SLEEP_MS(5);
      }

      // SLIDE window
      if (out_pos > 0) {
        if (out_pos == in_len) { out_pos = in_len = 0; }
        else if (out_pos >= BUF_CAP / 2) {
          const size_t remain = in_len - out_pos;
          memmove(buf.get(), buf.get() + out_pos, remain);
          in_len = remain; out_pos = 0;
        }
      }

      // FILL (strict clamp to file + body)
      size_t free_space = BUF_CAP - in_len;
      if (free_space > HEADROOM) {
        // bytes left in playable FILE (after baseOffset_)
        size_t fileRemaining = (totalSize_ >= 0) ? (totalSize_ - n) : std::numeric_limits<size_t>::max();
        if (fileRemaining <= 0) {
          // Clean EOF (file consumed)
          eofSeen_.store(true);
          targetUri_.clear();             // let outer loop advance to next track
          // drain and leave
          if (out_pos >= in_len) break;
          BELL_YIELD();
          continue;
        }

        // bytes left in this HTTP body
        size_t bodyRemaining = respRemaining;

        size_t to_read = (size_t)std::min<size_t>(std::min<size_t>(PULL_BYTES, free_space - HEADROOM), std::min<size_t>(fileRemaining, bodyRemaining));
        if (to_read == 0) {
          // body finished ⇒ EOF on wire
          if (resp->stream().isOpen()) {
            resp->drainBody();
            resp->stream().close();
          }
          if (eofMode_.load()) {
            eofSeen_.store(true);
            targetUri_.clear();
            if (out_pos >= in_len) break;
            BELL_YIELD();
          }
          else {
            // resume from logical byte n (CDN offset = baseOffset_ + n)
            resp = open_at(url, n + baseOffset_);
            if (!resp || !resp->stream().isOpen()) { SC32_LOG(error, "resume failed"); abortTrack = true; break; }
            int st3 = resp->status();
            if (st3 == 416) { eofSeen_.store(true); targetUri_.clear(); if (out_pos >= in_len) break; }
            else if (st3 != 206 && st3 != 200) { SC32_LOG(error, "resume HTTP %d", st3); abortTrack = true; break; }
            respRemaining = totalSize_ - n;
            if (respRemaining <= 0) respRemaining = std::numeric_limits<size_t>::max();
            BELL_YIELD();
          }
          continue;
        }
        if (!resp->stream().isOpen()) {
          resp = open_at(url, n + baseOffset_);
        }

        size_t got = 0;
        try {
          got = resp->readExact(buf.get() + in_len, to_read, 100);
        }
        catch (...) {
          SC32_LOG(error, "readSome threw");
          if (resp->stream().isOpen()) {
            resp->drainBody();
            resp->stream().close();
          }
          abortTrack = true;
          break;
        }
        if (got != to_read) {
          SC32_LOG(info,
            "short read got=%lu to_read=%lu respRemaining=%lu in_len=%lu buf_cap=%lu totalSize=%lu",
            got,
            (unsigned long)to_read,
            (unsigned long)respRemaining,
            (unsigned long)in_len,
            (unsigned long)BUF_CAP,
            (unsigned long)totalSize_);
          BELL_SLEEP_MS(5);
          resp->drainBody();
          resp->stream().close();
        }
        //respRemaining = got;
        in_len += (size_t)got;
        if (respRemaining != std::numeric_limits<size_t>::max())
          respRemaining -= (size_t)got;
      }
      BELL_SLEEP_MS(1);
    } // inner while

    if (resp && resp->stream().isOpen()) {
      resp->drainBody();
      resp->stream().close();
    }
    if (wantStop_.load()) wantStop_.store(false);
    if (abortTrack) { wantRestart_.store(true); retries--; }
    else if (repeatOne_.load()) {
      wantRestart_.store(true);
    }
    else if (eofMode_.load()) retries = 0;
  } // outer while
  feed_->feedCommand(AudioControl::DISC, 0);
  while (hb_) BELL_SLEEP_MS(100);
  isRunning_.store(false);
}

void QobuzPlayer::sendPlayerState() {
  qconnect_QConnectMessage msg = qconnect_QConnectMessage_init_zero;
  msg.has_messageType = true;
  msg.messageType = qconnect_QConnectMessageType_MESSAGE_TYPE_RNDR_SRVR_STATE_UPDATED;
  msg.has_rndrSrvrStateUpdated = true;
  msg.rndrSrvrStateUpdated.has_state = true;
  msg.rndrSrvrStateUpdated.state = player_state;
  msg.rndrSrvrStateUpdated.state.currentPosition.has_timestamp = true;
  msg.rndrSrvrStateUpdated.state.currentPosition.timestamp = timesync::now_ms();
  if (player_state.currentPosition.timestamp != 0) {
    msg.rndrSrvrStateUpdated.state.currentPosition.has_value = true;
    if (player_state.playingState == qconnect_PlayingState_PLAYING_STATE_PLAYING) msg.rndrSrvrStateUpdated.state.currentPosition.value = msg.rndrSrvrStateUpdated.state.currentPosition.timestamp - player_state.currentPosition.timestamp + player_state.currentPosition.value;
  }
  else msg.rndrSrvrStateUpdated.state.currentPosition.has_value = false;
  on_ws_msg_(&msg, 1);
}

bool QobuzPlayer::probeFlac(bell::HTTPClient::Response* s, size_t n, uint8_t* dst, size_t& offset)
{
  constexpr size_t WIN = PROBE_MAX;     // 1024
  constexpr size_t OVERLAP = 14;
  constexpr size_t STEP = WIN - OVERLAP; // 1010
  constexpr size_t MAX_EXTRA = 3072;
  uint8_t fileHeader[4];
  size_t filled = s->readExact(fileHeader, 4);
  if (filled != 4) { SC32_LOG(info, "probe short %lu", filled); return false; }
  // functional flac header
  if (memcmp(fileHeader, "fLaC", 4) == 0) {
    filled += fetch_flac_metadata(s, dst, n);
    offset = filled;
    return true;
  }

  uint8_t probe[WIN];
  memcpy(probe, fileHeader, 4);
  filled += s->readExact(probe + 4, WIN - 4);
  if (filled != WIN) { SC32_LOG(info, "probe short %lu", filled); return false; }

  auto find_sync = [&](const uint8_t* p, size_t n)->int {
    for (size_t i = 0;i + 2 <= n;++i) if (sync_ff_f8fb(p + i, n - i)) return (int)i;
    return -1;
    };

  size_t base_abs = 0;
  size_t extra = 0;
  for (;;) {
    int at = find_sync(probe, filled);
    if (at >= 0) {
      // Parse params from frame header
      uint32_t sr = 0; uint8_t ch = 0; uint8_t bps = 0; uint16_t bs = 4096;
      const uint8_t* hdr = probe + at; // hdr[0] == 0xFF
      size_t have_after = filled - (size_t)at - 1; // bytes after 0xFF already in buffer
      if (!parse_flac_frame_header(s, hdr, have_after, sr, ch, bps, bs)) return false;

      if (!sr)  sr = current_track_buffering->sampling_rate ? current_track_buffering->sampling_rate : 44100;
      if (!bps) bps = current_track_buffering->bits_depth ? current_track_buffering->bits_depth : 16;

      size_t total_samples = 0; // unknown is fine
      create_flac_metadata(dst, sr, ch ? ch : 2, bps, total_samples, bs ? bs : 4096);
      SC32_LOG(info, "flac sr %u ch %u bps %u bs %u", sr, ch, bps, bs);
      offset = base_abs + (size_t)at;
      return true;
    }

    if (extra >= MAX_EXTRA) { SC32_LOG(info, "no sync within budget"); return false; }

    // slide 14B, read next chunk
    memmove(probe, probe + (WIN - OVERLAP), OVERLAP);
    base_abs += STEP;
    filled = OVERLAP;

    size_t want = std::min(STEP, MAX_EXTRA - extra);
    size_t got = s->readExact(probe + filled, want);
    if (got == 0) return false;
    filled += got;
    extra += got;
  }
}

// --- event helpers ---

static inline std::string build_start_event(std::shared_ptr<qobuz::QobuzQueueTrack> track,
  const std::string& user_id, int played_for_s) {
  std::ostringstream oss;
  oss << "events=[{"
    << "\"user_id\":" << user_id << ","
    << "\"track_id\":" << track->id << ","
    << "\"format_id\":" << (int)track->format << ","
    << "\"date\":" << timesync::now_s_text(0) << ","
    << "\"duration\":" << played_for_s << ","
    << "\"online\":true,"
    << "\"local\":false"
    << "}]";
  return oss.str();
}

static inline std::string iso8601_ms_z_from_epoch_ms(uint64_t epoch_ms) {
  const std::time_t sec = static_cast<std::time_t>(epoch_ms / 1000ULL);
  const unsigned ms = static_cast<unsigned>(epoch_ms % 1000ULL);
  struct tm tm_utc;
#if defined(_WIN32)
  gmtime_s(&tm_utc, &sec);
#else
  gmtime_r(&sec, &tm_utc);
#endif
  char date[21];
  if (std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &tm_utc) == 0) {
    return "1970-01-01T00:00:00.000Z";
  }
  char out[32];
  std::snprintf(out, sizeof(out), "%s.%03uZ", date, ms);
  return std::string(out);
}
static inline std::string build_end_event(std::shared_ptr<qobuz::QobuzQueueTrack> track,
  const std::string& user_id, int played_for_s) {
  std::ostringstream oss;
  oss << "{\"events\":[{"
    << "\"blob\":\"" << track->blob << "\","
    << "\"track_context_uuid\":\"" << track->contextUuid << "\","
    << "\"start_stream\":\"" << iso8601_ms_z_from_epoch_ms(track->startedPlayingAt) << "\","
    << "\"online\":true,"
    << "\"local\":false,"
    << "\"duration\":" << played_for_s
    << "}],\"renderer_context\":{\"software_version\":\"sc32-1.0.0\"}}";
  return oss.str();
}
// --- HTTP helpers ---

static inline bool check_http_status(const uint16_t status) {
  return status >= 200 && status < 300;
}

static inline size_t parse_content_range_total(const std::string& cr) {
  if (cr.rfind("bytes ", 0) != 0) return 0;
  size_t sp = 6;
  size_t dash = cr.find('-', sp); if (dash == std::string::npos) return 0;
  size_t slash = cr.find('/', dash + 1); if (slash == std::string::npos) return 0;
  return std::stoull(cr.substr(slash + 1));
}

static inline std::unique_ptr<bell::HTTPClient::Response> open_at(const std::string& url, std::optional<size_t> pos, bool keepAlive) {
  bell::HTTPClient::Headers hdrs = {
    {"Accept","audio/*"},{"Accept-Encoding","identity"},
    {"User-Agent","StreamCore32/1.0"}
  };
  if (pos) hdrs.emplace_back("Range", "bytes=" + std::to_string(*pos) + "-");
  return bell::HTTPClient::get(url, hdrs, keepAlive);
}
// --- AudioFile helpers ---
static inline size_t ms_to_offset(size_t fileSize, size_t duration_ms, size_t pos_ms) {
  if (!pos_ms) return 0;
  return (size_t)(1.0 * pos_ms / duration_ms * fileSize);
}

// ---- QobuzFlacHeaders ----
// ---- tiny helpers ----
static inline void be16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
static inline void be24(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 16); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v); }
static inline void be64(uint8_t* p, uint64_t v) {
  for (int i = 7; i >= 0; --i) { p[7 - i] = uint8_t(v >> (i * 8)); }
}

// Build a 34-byte STREAMINFO from known params.
// total_samples can be 0 if unknown (many decoders are fine with that).
static inline void create_flac_metadata(
  uint8_t* dst,
  uint32_t sample_rate,
  uint8_t  channels,      // 1..8
  uint8_t  bits_per_sample, // 4..32
  size_t total_samples, // 0 = unknown
  uint16_t block_size)
{

  memcpy(dst, "fLaC", 4);
  dst[4] = 0x80 | 0x00; // is_last = 1 | type = STREAMINFO
  dst[5] = 0x00; dst[6] = 0x00; dst[7] = 0x22; // 34 bytes
  // min/max block size
  be16(&dst[8], block_size);
  be16(&dst[10], block_size);

  // min/max frame size unknown
  be24(&dst[12], 0);
  be24(&dst[15], 0);

  // Pack: [20 bits sr][3 bits (ch-1)][5 bits (bps-1)][36 bits total_samples]
  const uint32_t sr20 = sample_rate & 0xFFFFF;
  const uint64_t ch3 = uint64_t((channels ? channels : 2) - 1) & 0x7;
  const uint64_t bps5 = uint64_t((bits_per_sample ? bits_per_sample : 16) - 1) & 0x1F;
  const uint64_t ts36 = (uint64_t)total_samples & ((1ULL << 36) - 1);

  uint64_t packed = ((((uint64_t)sr20 << 3) | ch3) << 5 | bps5) << 36 | ts36;
  be64(&dst[18], packed);
  memset(&dst[26], 0, 16);       // MD5 = 0
}
static inline bool sync_ff_f8fb(const uint8_t* p, size_t n) {
  return n >= 2 && p[0] == 0xFF && (p[1] & 0xF8) == 0xF8;
}


// Decode FLAC frame header fields needed for STREAMINFO.
// Returns true if we got sr/ch/bps/blocksize.
// Reads extra bytes from the socket if header uses extended SR/BS codes.
static bool parse_flac_frame_header(bell::HTTPClient::Response* s,
  const uint8_t* buf, size_t have_after_sync,
  uint32_t& sr, uint8_t& ch, uint8_t& bps, uint16_t& bs)
{
  // We assume buf[0..] points at the *second* byte of the sync (i.e., buf[-1]==0xFF, buf[0]==0xF8..FB)
  // Layout after sync:
  //   B0: 11111xxx  (already checked)
  //   B1: [rsvd(1)][blocking(1)][blocksize_code(4)]
  //   B2: [sample_rate_code(4)][channel_assign(4)]
  //   B3: [sample_size_code(3)][rsvd(1)][UTF8-coded sample/frame number begins here...]
  // We only need codes; we may need extra trailing bytes for blocksize/sample-rate when codes 6.. etc.
  if (have_after_sync < 3) return false;

  const uint8_t b1 = buf[1];                 // after the 0xFF
  const uint8_t b2 = buf[2];
  const uint8_t b3 = buf[3];

  const uint8_t bs_code = (b1 & 0x0F);
  const uint8_t sr_code = (b2 >> 4) & 0x0F;
  const uint8_t ch_code = (b2 & 0x0F);
  const uint8_t sz_code = (b3 >> 4) & 0x07;

  // --- channels ---
  if (ch_code <= 7) ch = ch_code + 1;
  else ch = 2; // left/side, right/side, mid/side → 2 channels

  // --- bits-per-sample ---
  // 000: from STREAMINFO, 001:8, 010:12, 011:reserved, 100:16, 101:20, 110:24, 111: from STREAMINFO
  switch (sz_code) {
  case 1: bps = 8;  break;
  case 2: bps = 12; break;
  case 4: bps = 16; break;
  case 5: bps = 20; break;
  case 6: bps = 24; break;
  default: bps = 0; break; // unknown → use metadata fallback later
  }

  // --- blocksize ---
  // 0001:192, 0010:576, 0011:1152, 0100:2304, 0101:4608
  // 0110: get 8-bit blocksize-1, 0111: get 16-bit blocksize-1
  // 1000..1111: 256,512,1024,...,32768 (2^(N-8+8)=2^N?)
  switch (bs_code) {
  case 1:  bs = 192; break;
  case 2:  bs = 576; break;
  case 3:  bs = 1152; break;
  case 4:  bs = 2304; break;
  case 5:  bs = 4608; break;
  case 6: { // 8-bit (blocksize-1)
    uint8_t ext;
    if (have_after_sync >= 4) ext = buf[4];
    else {
      if (s->readExact(&ext, 1) != 1) return false;
    }
    bs = uint16_t(ext) + 1;
  } break;
  case 7: { // 16-bit (blocksize-1)
    uint8_t ext[2];
    size_t need = 2;
    size_t have = (have_after_sync >= 5) ? std::min<size_t>(have_after_sync - 4, 2) : 0;
    if (have) memcpy(ext, buf + 4, have);
    if (have < need) {
      if (s->readExact(ext + have, need - have) != (int)(need - have)) return false;
    }
    bs = (uint16_t(ext[0]) << 8 | ext[1]) + 1;
  } break;
  default: {
    // 8..15: 256<< (bs_code-8)
    if (bs_code >= 8) bs = uint16_t(256u << (bs_code - 8));
    else bs = 4096;
  } break;
  }

  // --- sample-rate ---
  // 0000: from STREAMINFO
  // 0001: 88.2k, 0010:176.4k, 0011:192k,
  // 0100: 8k, 0101:16k, 0110:22.05k, 0111:24k,
  // 1000: 32k, 1001:44.1k, 1010:48k, 1011:96k,
  // 1100: 8-bit (kHz), 1101: 16-bit (Hz), 1110: 16-bit (10*Hz), 1111: reserved
  switch (sr_code) {
  case  1: sr = 88200; break;  case  2: sr = 176400; break; case  3: sr = 192000; break;
  case  4: sr = 8000;  break;  case  5: sr = 16000;  break; case  6: sr = 22050;  break;
  case  7: sr = 24000; break;  case  8: sr = 32000;  break; case  9: sr = 44100;  break;
  case 10: sr = 48000; break;  case 11: sr = 96000;  break;
  case 12: { // 8-bit kHz
    uint8_t x;
    // find where the ext byte lives: it comes *after* the UTF-8 coded frame/sample number.
    // We don't need that number; just read forward one byte at a time until CRC8 would appear…
    // Pragmatic shortcut: read one byte now; most encoders put SR ext immediately after b3+UTF8(1).
    if (s->readExact(&x, 1) != 1) return false;
    sr = uint32_t(x) * 1000u;
  } break;
  case 13: { // 16-bit Hz
    uint8_t x[2];
    if (s->readExact(x, 2) != 2) return false;
    sr = (uint32_t(x[0]) << 8) | x[1];
  } break;
  case 14: { // 16-bit (10*Hz)
    uint8_t x[2];
    if (s->readExact(x, 2) != 2) return false;
    sr = ((uint32_t(x[0]) << 8) | x[1]) * 10u;
  } break;
  default: sr = 0; break; // from STREAMINFO / reserved
  }
  return true;
}
static inline size_t fetch_flac_metadata(bell::HTTPClient::Response* s, uint8_t* dst, size_t n) {
  size_t bytesRead = 0;
  bool isLast = false;
  while (!isLast && bytesRead < n) {
    uint8_t bh[4];
    size_t got = s->readExact(bh, 4);
    if (got != 4) { SC32_LOG(info, "probe short %lu", bytesRead); return false; }
    bytesRead += got;
    isLast = bh[0] & 0x80;
    const uint8_t btype = bh[0] & 0x7f;
    uint32_t bsize = (uint32_t)bh[1] << 16 | (uint32_t)bh[2] << 8 | (uint32_t)bh[3];

    switch (btype) {
    case 0:
      // STREAMINFO
      if (bsize == 34) {
        memcpy(dst, "fLaC", 4);
        dst[4] = 0x80 | 0x00; // is_last = 1 | type = STREAMINFO
        dst[5] = 0x00; dst[6] = 0x00; dst[7] = 0x22; // 34 bytes
        uint8_t buf[bsize];
        got = s->readExact(buf, bsize);
        if (got != bsize) { SC32_LOG(info, "probe short %lu", bytesRead); return false; }
        bytesRead += got;
        memcpy(dst + 8, buf, 34);
        break;
      }
      [[fallthrough]]; // STREAMINFO
      /*
      case 1: break; // PADDING
      case 2: break; // APPLICATION
      case 3: break; // SEEKTABLE
      case 4: break; // VORBIS_COMMENT
      case 5: break; // CUESHEET
      case 6: break; // PICTURE
      */
    default:
      //SC32_LOG(info, "type %u size %u", btype, bsize);
      while (bsize > 0) {
        size_t want = std::min((size_t)bsize, PROBE_MAX);
        uint8_t buf[want];
        size_t got = s->readExact(buf, want);
        if (got != want) { SC32_LOG(info, "probe short %lu", bytesRead); return false; }
        bytesRead += got;
        bsize -= want;
      }
      break;
    }
  }
  return bytesRead;
}
