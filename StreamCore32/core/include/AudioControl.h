#pragma once

#include <stddef.h>    // for size_t
#include <stdint.h>    // for uint8_t
#include <atomic>      // for atomic
#include <functional>  // for function
#include <memory>      // for shared_ptr, unique_ptr
#include <mutex>       // for mutex
#include <string>      // for string
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#ifdef CONFIG_AUDIO_SINK_VS1053
#include "VS1053.h"  // for VS1053
#define _AudioSink VS1053
#else
#include "StreamOrientedAudioSink.h"
#define _AudioSink StreamOrientedAudioSink
#endif

class AudioControl {
 public:
  enum CommandType {
    PLAY,
    PAUSE,
    DISC,
    DEPLETED,
    FLUSH,
    SKIP,
    VOLUME_LINEAR,
    VOLUME_LOGARITHMIC
  };
  class FeedControl {
   public:
    FeedControl(std::shared_ptr<AudioControl> audioController) {
      this->audioController = audioController;
      this->audioSink = audioController->audioSink;
    };
    std::shared_ptr<_AudioSink> audioSink = NULL;
    std::shared_ptr<AudioControl> audioController;
    size_t feedData(uint8_t* data, size_t bytes, size_t trackId,
                    bool STORAGE_VOLATILE = 0);
    template <class T>
    void feedCommand(CommandType command, T value,
                     std::optional<T> limit = std::nullopt) {
      if (command != CommandType::VOLUME_LINEAR &&
          command != CommandType::VOLUME_LOGARITHMIC) {
        if (!this->audioSink->streams.size())
          return;
      }
      switch (command) {
        case CommandType::PLAY:
          if (this->audioSink->streams[0]->state !=
              _AudioSink::Stream::State::Stopped)
            this->audioSink->new_state(this->audioSink->streams[0],
                                       _AudioSink::Stream::State::Playback);
          break;

        case CommandType::PAUSE:
          if (this->audioSink->streams[0]->state !=
              _AudioSink::Stream::State::Stopped)
            this->audioSink->new_state(
                this->audioSink->streams[0],
                _AudioSink::Stream::State::PlaybackPaused);
          break;

        case CommandType::DISC: {
          printf("DISC\n");
          auto& v = this->audioSink->streams;
          if (!v.size())
            break;
          if (v.size() > 1) {
            auto it = v.begin() + 1;  // start after index 0
            while (it != v.end()) {
              // adapt *it access to your actual element type (ptr/unique_ptr/object)
              auto* src = static_cast<FeedControl*>((*it)->source);
              if (src == this) {
                it = v.erase(it);  // erase returns the next iterator
              } else {
                ++it;
              }
            }
          }
          if (!v.empty() && v[0]->source == this) {  // guard against empty
            printf("DISC stop\n");
            this->audioSink->stop_feed();
          }
        } break;

        case CommandType::FLUSH:
          if (this->audioSink->streams.size() > 0)
            this->audioSink->streams[0]->empty_feed();
          break;

        case CommandType::SKIP:
          this->audioSink->stop_feed();
          break;

        case CommandType::VOLUME_LINEAR: {
          uint8_t lin = limit.has_value()
                            ? this->audioSink->to_linear_volume(value, limit)
                            : this->audioSink->to_linear_volume(value);
          this->audioController->volume.store(lin);
          printf("VOLUME_LINEAR %d\n", this->audioController->volume.load());
          this->audioSink->feed_command([this](uint8_t) {
            this->audioSink->set_volume(this->audioController->volume.load());
            printf("VOLUME_LINEAR %d\n", this->audioController->volume.load());
          });
        } break;
        case CommandType::VOLUME_LOGARITHMIC: {
          this->audioController->volume.store(
              this->audioSink->to_logarithmic_volume(value));
          this->audioSink->feed_command([this, value](uint8_t) {
            this->audioSink->set_volume(this->audioController->volume.load());
          });
        } break;

        default:
          break;
      }
    }
    std::function<void(uint8_t)> state_callback = [](uint8_t state) {
    };
  };
  AudioControl();
  size_t getHeaderOffset(size_t trackId);

  void setParams(size_t sampleRate, size_t channels, size_t bitsPerSample) {
    this->audioSink->setParams(sampleRate, channels, bitsPerSample);
  }
  size_t makeUniqueTrackId() {
    this->trackId++;
    return this->trackId;
  }

  std::atomic<size_t> volume = 90;
  SemaphoreHandle_t SPI_semaphore;

 private:
  size_t trackId = 0;
  std::shared_ptr<_AudioSink> audioSink;

  std::atomic<bool> pauseRequested = false;
  std::atomic<bool> isPaused = true;
  std::atomic<bool> isRunning = true;
  std::atomic<bool> playlistEnd = false;
};
