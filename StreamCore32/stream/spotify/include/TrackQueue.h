#pragma once

#include <stddef.h>  // for size_t
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>  // for pair

#include "BellTask.h"
#include "EventManager.h"  // for TrackMetrics
#include "TrackReference.h"
#include "Utils.h"

#include "WrappedSemaphore.h"
#include "protobuf/connect.pb.h"   // for ProvidedTrack
#include "protobuf/metadata.pb.h"  // for Track, _Track, AudioFile, Episode

namespace spotify {
struct Context;
class AccessKeyFetcher;
class CDNAudioFile;

// Used in got track info event
struct TrackInfo {
  std::string name, album, artist, imageUrl, trackId, provider,
      page_instance_id, interaction_id, decision_id;
  uint32_t duration, number, discNumber;
  void loadPbTrack(Track* pbTrack, const std::vector<uint8_t>& gid);
  void loadPbEpisode(Episode* pbEpisode, const std::vector<uint8_t>& gid);
};

class QueuedTrack {
 public:
  QueuedTrack(player_proto_connect_ProvidedTrack& ref,
              std::shared_ptr<spotify::Context> ctx,
              std::shared_ptr<bell::WrappedSemaphore> playableSemaphore,
              int64_t requestedPosition = 0);
  ~QueuedTrack();

  enum class State {
    QUEUED,
    PENDING_META,
    KEY_REQUIRED,
    PENDING_KEY,
    CDN_REQUIRED,
    READY,
    LOADED,
    PLAYING,
    PAUSED,
    STOPPED,
    FINISHED,
    FAILED
  };

  State state = State::QUEUED;  // Current state of the track
  TrackInfo trackInfo;  // Full track information fetched from spotify, name etc
  player_proto_connect_ProvidedTrack ref;
  std::string identifier;
  uint32_t playingTrackIndex;
  uint32_t requestedPosition;
  AudioFormat audioFormat;
  bool loading = false;
  uint8_t retries = 0;

  // PB data
  Track pbTrack = Track_init_zero;
  Episode pbEpisode = Episode_init_zero;

  // EventManager data
  int64_t written_bytes = 0;
  std::shared_ptr<spotify::TrackMetrics> trackMetrics;

  // Will return nullptr if the track is not ready
  std::shared_ptr<spotify::CDNAudioFile> getAudioFile();

  void cancelLoading();
  // --- Steps ---
  void stepLoadMetadata(
      Track* pbTrack, Episode* pbEpisode, std::mutex& trackListMutex,
      std::shared_ptr<bell::WrappedSemaphore> updateSemaphore);

  bool stepParseMetadata(Track* pbTrack, Episode* pbEpisode);

  void stepLoadAudioFile(
      std::mutex& trackListMutex,
      std::shared_ptr<bell::WrappedSemaphore> updateSemaphore);

  void stepLoadCDNUrl(const std::string& accessKey);

 private:
  std::shared_ptr<spotify::Context> ctx;
  std::shared_ptr<bell::WrappedSemaphore> playableSemaphore;

  uint64_t pendingMercuryRequest = 0;
  uint32_t pendingAudioKeyRequest = 0;

  std::vector<uint8_t> trackId, fileId, audioKey;
  std::string cdnUrl;
  std::pair<SpotifyFileType, std::vector<uint8_t>> gid = {
      SpotifyFileType::UNKNOWN,
      {}};
};

class TrackQueue : public bell::Task {
 public:
  TrackQueue(std::shared_ptr<spotify::Context> ctx);
  ~TrackQueue();

  enum class SkipDirection { NEXT, PREV };

  std::shared_ptr<bell::WrappedSemaphore> playableSemaphore;
  std::shared_ptr<spotify::AccessKeyFetcher> accessKeyFetcher;
  std::atomic<bool> notifyPending = false;
  std::deque<std::shared_ptr<QueuedTrack>> preloadedTracks;
  bool repeat = false;

  void runTask() override;
  void stopTask();

  bool skipTrack(SkipDirection dir, bool expectNotify = false);
  TrackInfo getTrackInfo(std::string_view identifier);
  std::shared_ptr<QueuedTrack> consumeTrack(
      std::shared_ptr<QueuedTrack> prevSong, int& offset);
  std::mutex tracksMutex, runningMutex;

 private:
  std::shared_ptr<spotify::Context> ctx;
  std::shared_ptr<bell::WrappedSemaphore> processSemaphore;

  std::atomic<bool> isRunning = false;

  std::string accessKey;

  bool processTrack(std::shared_ptr<QueuedTrack> track);
};
}  // namespace spotify