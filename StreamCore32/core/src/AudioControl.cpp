#include "AudioControl.h"

#include <BellLogger.h>  // for BELL_LOG
#include <cstdint>       // for uint8_t
#include <iostream>      // for operator<<, basic_ostream, endl, cout
#include <memory>        // for shared_ptr, make_shared, make_unique
#include <mutex>         // for scoped_lock
#include <variant>       // for get

AudioControl::AudioControl() {
  BELL_LOG(info, "STREAMCONTROL", "AudioControl::AudioControl()");
  SPI_semaphore = xSemaphoreCreateMutex();
  assert(SPI_semaphore != NULL);
  BELL_LOG(info, "STREAMCONTROL", "AudioControl::AudioControl() end");
  this->audioSink = std::make_shared<_AudioSink>(&SPI_semaphore);  //audioSink;
  this->audioSink->setParams(44100, 2, 16);
  this->audioSink->set_volume(volume.load());
  this->audioSink->state_callback = [this](_AudioSink::Stream::State state,
                                           void* source) {
    if (source == NULL)
      return;
    if (state == _AudioSink::Stream::State::Playback) {
      if (this->isRunning.load() == false)
        this->isRunning.store(true);
      this->isPaused.store(false);
    } else if (state == _AudioSink::Stream::State::PlaybackPaused) {
      this->isPaused.store(true);
    } else if (state == _AudioSink::Stream::State::Stopped &&
               this->audioSink->streams.size() == 1) {
      this->isRunning.store(false);
    }
    static_cast<FeedControl*>(source)->state_callback((uint8_t)state);
  };
}
size_t AudioControl::FeedControl::feedData(uint8_t* data, size_t bytes,
                                           size_t streamId,
                                           bool STORAGE_VOLATILE) {
  auto it =
      std::find_if(audioSink->streams.begin(), audioSink->streams.end(),
                   [streamId](const std::shared_ptr<_AudioSink::Stream>& s) {
                     return s && s->streamId == streamId;
                   });

  std::shared_ptr<_AudioSink::Stream> stream =
      (it != audioSink->streams.end()) ? *it : nullptr;
  if (stream == nullptr) {
    stream = std::make_shared<_AudioSink::Stream>(
        this, streamId,
        CONFIG_STREAM_BUFFER_SIZE);  // Recomended buffer size is a multiple of 1024
    audioSink->new_stream(stream);
    this->audioController->trackId = streamId;
    BELL_LOG(info, "FeedControl", "New streamId (%d)", streamId);
  }
  if (streamId != audioSink->streams[0]->streamId) {
    if (streamId > audioSink->streams[0]->streamId) {
      if (audioSink->streams[0]->state != _AudioSink::Stream::State::Stopped &&
          audioSink->streams[0]->state > _AudioSink::Stream::State::Playback)
        audioSink->soft_stop_feed();
    } else
      return bytes;
  }
  return stream->feed_data(data, bytes, STORAGE_VOLATILE);
}

size_t AudioControl::getHeaderOffset(size_t trackId) {
  return this->audioSink->stream_seekable(trackId);
}