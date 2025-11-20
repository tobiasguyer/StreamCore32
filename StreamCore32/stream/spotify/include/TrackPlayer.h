#pragma once

#include <atomic>       // for atomic
#include <cstdint>      // for uint8_t, int64_t
#include <ctime>        // for size_t, time
#include <functional>   // for function
#include <memory>       // for shared_ptr, unique_ptr
#include <mutex>        // for mutex
#include <string_view>  // for string_view
#include <vector>       // for vector

#include "BellTask.h"  // for Task
#include "CDNAudioFile.h"

namespace bell {
class WrappedSemaphore;
}  // namespace bell

#ifndef CONFIG_BELL_NOCODEC
#ifdef BELL_VORBIS_FLOAT
#include "vorbis/vorbisfile.h"
#else
#include "ivorbisfile.h"  // for OggVorbis_File, ov_callbacks
#endif
#endif

namespace spotify {
class TrackProvider;
class TrackQueue;
class QueuedTrack;
struct Context;
struct TrackReference;

class TrackPlayer : bell::Task {
 public:  
 enum class State {
    STOPPED,
    PLAYING,
    SEEKING,
    FAILED,
  };
  // Callback types
  typedef std::function<void(std::shared_ptr<spotify::QueuedTrack>, State)> StateChangedCallback;
  typedef std::function<size_t(uint8_t*, size_t, size_t, bool)> DataCallback;
  typedef std::function<size_t(size_t)> SeekableCallback;

  

  TrackPlayer(std::shared_ptr<spotify::Context> ctx, std::shared_ptr<spotify::TrackQueue> trackQueue, StateChangedCallback onStateChange, bool* track_repeat);
  ~TrackPlayer();

  void loadTrackFromRef(TrackReference& ref, size_t playbackMs,
                        bool startAutomatically);
  void setDataCallback(DataCallback callback,
                       SeekableCallback seekable_callback = nullptr,
                       SeekableCallback spaces_available = nullptr);

  // CDNTrackStream::TrackInfo getCurrentTrackInfo();
  void seekMs(size_t ms, bool loading = true);
  void resetState(bool paused = false);
  std::function<size_t(uint8_t*, size_t, size_t, bool)> dataCallback = nullptr;
  SeekableCallback headerSize;
#ifndef CONFIG_BELL_NOCODEC
  // Vorbis codec callbacks
  size_t _vorbisRead(void* ptr, size_t size, size_t nmemb);
  size_t _vorbisClose();
  int _vorbisSeek(int64_t offset, int whence);
  long _vorbisTell();
#endif

  void stop();
  void start();

 private:
  std::shared_ptr<spotify::Context> ctx;
  std::shared_ptr<spotify::TrackQueue> trackQueue;
  std::shared_ptr<spotify::CDNAudioFile> currentTrackStream;

  std::unique_ptr<bell::WrappedSemaphore> playbackSemaphore;

  StateChangedCallback setState;
#ifdef CONFIG_BELL_NOCODEC
  SeekableCallback spaces_available = nullptr;
  SeekableCallback seekable_callback;
  size_t seekable_offset;
#endif

  // Playback control
  std::atomic<bool> currentSongPlaying;
  bool* repeating_track_;
  std::mutex playbackMutex;
  std::mutex dataOutMutex;

#ifndef CONFIG_BELL_NOCODEC
  // Vorbis related
  OggVorbis_File vorbisFile;
  ov_callbacks vorbisCallbacks;
  int currentSection;
#endif

  std::vector<uint8_t> pcmBuffer = std::vector<uint8_t>(1024 * 12);

  bool autoStart = false;

  std::atomic<bool> isRunning = false;
  std::atomic<bool> pendingReset = false;
  std::atomic<bool> inFuture = false;
  std::atomic<size_t> pendingSeekPositionMs = 0;
  std::atomic<bool> startPaused = false;

  std::mutex runningMutex;

  void runTask() override;
};
}  // namespace spotify
