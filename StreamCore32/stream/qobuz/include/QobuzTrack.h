#pragma once

#include <string>
#include <stdint.h>
#include <atomic>

#include "NanoPBHelper.h"
#include "protobuf/qconnect_common.pb.h"

namespace qobuz {

  enum AudioFormat : uint8_t {
    QOBUZ_QUEUE_FORMAT_MP3 = 5,
    QOBUZ_QUEUE_FORMAT_FLAC_LOSSLESS = 6,
    QOBUZ_QUEUE_FORMAT_FLAC_HI_RES_96 = 7,
    QOBUZ_QUEUE_FORMAT_FLAC_HI_RES_192 = 27
  };

  enum QueuedTrackState {
    QUEUED,
    PENDING_META,
    STREAMABLE,
    PENDING_FILE,
    READY,
    LOADED,
    PLAYING,
    PAUSED,
    STOPPED,
    FINISHED,
    FAILED
  };

  struct QobuzAlbum {
    size_t qobuz_id;
    std::string name;
    std::string url;
    std::string id;
    struct Image {
      std::string thumbnail;
      std::string small_img;
      std::string large_img;
    } image;
    size_t genre_id = 0;
    size_t label_id = 0;
  };

  struct QobuzArtist {
    size_t id;
    std::string name;
  };

  class QobuzQueueTrack {
  public:
    QobuzQueueTrack(qconnect_QueueTrackRef track) {
      id = track.trackId;
      index = track.queueItemId;
      auto b = track.contextUuid->bytes;
      if(track.contextUuid){
        contextUuid.resize(37);
        snprintf(contextUuid.data(), 37,
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
      b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
      contextUuid.pop_back();
      }
      BELL_LOG(info, "queue", "QobuzQueueTrack: contextUuid=%s", contextUuid.c_str());
    };
    ~QobuzQueueTrack() {
    };

    // Requested output format
    AudioFormat format = AudioFormat::QOBUZ_QUEUE_FORMAT_FLAC_LOSSLESS;

    // Basic
    std::string title;
    std::string fileUrl;         // legacy direct URL (fmt 5/6)
    std::string contextUuid;
    QobuzArtist artist;
    QobuzAlbum album;
    size_t durationMs = 0;
    size_t startMs = 0;
    uint64_t startedPlayingAt = 0;
    size_t id = 0;
    size_t index = 0;
    std::atomic<bool> wantSkip_{ false };
    std::atomic<long long> skipTo_{ 0 };
    QueuedTrackState state = QueuedTrackState::QUEUED;

    // --- New: audio/transport metadata from secure/segmented response ---
    int format_id = 0;           // e.g. 7, 27
    std::string mime_type;       // e.g. "audio/mp4; codecs=\"flac\""
    int sampling_rate = 0;       // Hz
    int bits_depth = 0;          // 16/24
    int n_channels = 0;          // 2
    double duration_sec = 0.0;   // seconds
    size_t n_samples = 0;
    size_t audio_file_id = 0;
    std::string blob;          // opaque blob for auth

    struct Segmented {
      bool enabled = false;
      std::string url_template;  // ... s=$SEGMENT$ ...
      int n_segments = 0;
      std::string key_id;        // UUID
      std::string key;           // key material (base64-ish)
      std::string blob;          // opaque blob for auth
    } seg;

    // Helper: expand $SEGMENT$ into a concrete URL
    std::string segmentUrl(int s) const {
      if (!seg.enabled || seg.url_template.empty() || s < 0) return {};
      std::string u = seg.url_template;
      const char* token = "$SEGMENT$";
      size_t pos = u.find(token);
      if (pos != std::string::npos) {
        u.replace(pos, std::strlen(token), std::to_string(s));
      }
      return u;
    }

    // Helper: create json context object for suggestions
    std::string contextJson() const {
      std::string cj = "{";
      cj += "\"track_id\":" + std::to_string(id) + ",";
      if(artist.id > 0) cj += "\"artist_id\":" + std::to_string(artist.id) + ",";
      if(album.label_id > 0) cj += "\"label_id\":" + std::to_string(album.label_id) + ",";
      if(album.genre_id > 0) cj += "\"genre_id\":" + std::to_string(album.genre_id) + ",";
      cj.pop_back();
      return cj + "}";
    }
    bool isSegmented() const { return seg.enabled; }

    bool operator==(const QobuzQueueTrack& other) const {
      return id == other.id;
    }
  };
}
