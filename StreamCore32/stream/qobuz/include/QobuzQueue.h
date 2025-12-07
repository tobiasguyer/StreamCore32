#pragma once

#include <atomic>
#include <deque>  // for deque
#include <functional>
#include <memory>  // for shared_ptr
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "BellLogger.h"
#include "BellTask.h"
#include "HTTPClient.h"
#include "NanoPBHelper.h"

#include "protobuf/qconnect_payload.pb.h"

#include "EspRandomEngine.h"

#include "QobuzTrack.h"

namespace qobuz {

class QobuzQueue : public bell::Task {
 public:
  using OnWsMessage = std::function<void(_qconnect_QConnectMessage*, size_t)>;

  using OnQobuzGet = std::function<std::unique_ptr<bell::HTTPClient::Response>(
      const std::string& object,  //session, user, file
      const std::string& action,  // start, url...
      const std::vector<std::pair<std::string, std::string>>& params,
      bool sign)>;

  using OnQobuzPost = std::function<std::unique_ptr<bell::HTTPClient::Response>(
      const std::string& object, const std::string& action,
      const std::string& body,
      const std::vector<std::pair<std::string, std::string>>& params,
      bool sign)>;

  QobuzQueue(const uint8_t* session_id)
      : bell::Task("qobuz_queue", 4096 * 4, 0, 1, true) {
    memcpy(sessionId, session_id, 16);
  };
  ~QobuzQueue() {
    isRunning_.store(false);
    deleteQobuzTracks();
    pb_release(qconnect_SrvrCtrlQueueState_fields, &queueuState);
    std::scoped_lock lock(isRunningMutex_);
  };
  void setSessionId(const uint8_t* session_id) {
    memcpy(sessionId, session_id, 16);
  }
  void addQobuzTracks(qconnect_QueueTrackRef* tracks, size_t count,
                      std::optional<size_t> index = std::nullopt,
                      pb_bytes_array_t* uuid = nullptr) {
    std::scoped_lock lock(queueMutex_);
    if (index && *index < shuffledIndexes_.size())
      *index = shuffledIndexes_[*index];
    for (int i = 0; i < count; i++) {
      if (index) {
        bool alreadyExists = false;
        for (size_t j = index_; j < queue_.size(); j++) {
          if (tracks[i].trackId == queue_[j].trackId) {
            alreadyExists = true;
            int preloadedTrackIndex = j - index_;
            if (preloadedTrackIndex < preloadedTracks_.size())
              preloadedTracks_[preloadedTrackIndex]->index =
                  tracks[i].queueItemId;
            queue_[j].queueItemId = tracks[i].queueItemId;
            break;
          }
        }
        if (alreadyExists)
          continue;
        auto pos = queue_.insert(queue_.begin() + *index + 1,
                                 qconnect_QueueTrackRef_init_zero);
        PB_MOVE_ASSIGN(qconnect_QueueTrackRef, *pos,
                       tracks[i]);  // move ownership
        if (!pos->contextUuid && uuid) {
          pos->contextUuid = static_cast<pb_bytes_array_t*>(
              malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(uuid->size)));
          memcpy(pos->contextUuid->bytes, uuid->bytes, uuid->size);
          pos->contextUuid->size = uuid->size;
        }
        ++*index;
      } else {
        queue_.push_back(qconnect_QueueTrackRef_init_zero);
        PB_MOVE_ASSIGN(qconnect_QueueTrackRef, queue_.back(),
                       tracks[i]);  // move ownership
        if (!queue_.back().contextUuid && uuid) {
          queue_.back().contextUuid = static_cast<pb_bytes_array_t*>(
              malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(uuid->size)));
          memcpy(queue_.back().contextUuid->bytes, uuid->bytes, uuid->size);
          queue_.back().contextUuid->size = uuid->size;
        }
      }
    }
  }
  void updateQobuzTracks(qconnect_QueueTrackRef* tracks, size_t count,
                         pb_bytes_array_t* uuid = nullptr) {
    std::scoped_lock lock(queueMutex_);
    std::scoped_lock lock2(preloadedTracksMutex_);
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
      for (int i = 0; i < count; i++) {
        if (it->queueItemId == tracks[i].queueItemId) {
          if (it->contextUuid) {
            pb_free(it->contextUuid);
            it->contextUuid = nullptr;
          }
          if (!it->contextUuid && uuid) {
            it->contextUuid = static_cast<pb_bytes_array_t*>(
                malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(uuid->size)));
            memcpy(it->contextUuid->bytes, uuid->bytes, uuid->size);
            it->contextUuid->size = uuid->size;
          }
          it->queueItemId = tracks[i].queueItemId;
          if (preloadedTracks_[0]->id == tracks[i].queueItemId) {
            preloadedTracks_[0]->index = tracks[i].queueItemId;

            auto b = tracks[i].contextUuid->bytes;
            if (tracks[i].contextUuid) {
              preloadedTracks_[0]->contextUuid.resize(37);
              snprintf(preloadedTracks_[0]->contextUuid.data(), 37,
                       "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%"
                       "02x%02x%02x%02x",
                       b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8],
                       b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
              preloadedTracks_[0]->contextUuid.pop_back();
            }
          }
          break;
        }
      }
    }
  }
  size_t getRegularTracksSize() { return shuffledIndexes_.size(); }
  void deleteAutoplayTracks() {
    std::scoped_lock lock(queueMutex_);
    std::scoped_lock lock2(preloadedTracksMutex_);
    if (queue_.size() <= 1)
      return;
    if (fetchedAutoplay_) {
      fetchedAutoplay_ = false;
      for (size_t i = shuffledIndexes_.size(); i < queue_.size(); i++) {
        if (queue_[i].contextUuid) {
          pb_free(queue_[i].contextUuid);
          queue_[i].contextUuid = nullptr;
        }
      }
      addShuffleIndexes(queue_.size(), std::nullopt, false);
    } else {
      while (queue_.size() > shuffledIndexes_.size()) {
        pb_release(qconnect_QueueTrackRef_fields, &queue_.back());
        queue_.pop_back();
      }
    }
  }
  void deleteQobuzTracks(qconnect_QueueTrackRef* tracks = NULL,
                         size_t count = 0) {
    std::scoped_lock lock(queueMutex_);
    std::scoped_lock lock2(preloadedTracksMutex_);
    if (tracks == NULL) {
      for (auto& e : queue_) {
        pb_release(qconnect_QueueTrackRef_fields, &e);
      }
      queue_.clear();
      shuffledIndexes_.clear();
      index_ = 0;
      preloadedTracks_.clear();
      expandedTrackInfo_cache_.clear();
    } else {
      for (size_t i = 0; i < count; ++i) {
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
          if (it->trackId == tracks[i].trackId) {
            if (preloadedTracks_.size()) {
              for (auto pit = preloadedTracks_.begin();
                   pit != preloadedTracks_.end(); ++pit) {
                if ((*pit)->id == tracks[i].trackId) {
                  preloadedTracks_.erase(pit);
                  break;
                }
              }
            }
            size_t index = it - queue_.begin();
            auto sit = std::find(shuffledIndexes_.begin(),
                                 shuffledIndexes_.end(), index);
            if (sit != shuffledIndexes_.end()) {
              shuffledIndexes_.erase(sit);
              for (size_t j = 0; j < shuffledIndexes_.size(); j++) {
                if (shuffledIndexes_[j] > index)
                  shuffledIndexes_[j]--;
              }
              if (index_ >= index)
                index_--;
            }
            pb_release(qconnect_QueueTrackRef_fields, &*it);
            queue_.erase(it);
            break;
          }
        }
      }
    }
  }
  void deleteQobuzTracks(uint32_t* tracks, size_t count) {
    std::scoped_lock lock(queueMutex_);
    std::scoped_lock lock2(preloadedTracksMutex_);
    for (int i = 0; i < count; i++) {
      for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (it->queueItemId == tracks[i]) {
          if (preloadedTracks_.size()) {
            for (auto pit = preloadedTracks_.begin();
                 pit != preloadedTracks_.end(); ++pit) {
              if ((*pit)->index == it->queueItemId) {
                preloadedTracks_.erase(pit);
                break;
              }
            }
          }
          size_t index = it - queue_.begin();
          auto sit = std::find(shuffledIndexes_.begin(), shuffledIndexes_.end(),
                               index);
          if (sit != shuffledIndexes_.end()) {
            shuffledIndexes_.erase(sit);
            for (size_t j = 0; j < shuffledIndexes_.size(); j++) {
              if (shuffledIndexes_[j] > index)
                shuffledIndexes_[j]--;
            }
            if (index_ >= index)
              index_--;
          }
          pb_release(qconnect_QueueTrackRef_fields, &*it);
          queue_.erase(it);
          break;
        }
      }
    }
  }
  bool position(size_t& index, size_t queueItemId) {
    for (size_t i = 0; i < queue_.size(); i++) {
      if (queue_[i].queueItemId == queueItemId) {
        if (!shuffledIndexes_.empty() && i < shuffledIndexes_.size()) {
          for (size_t j = 0; j < shuffledIndexes_.size(); j++) {
            if (shuffledIndexes_[j] == i) {
              index = j;
              return true;
            }
          }
        }
        index = i;
        return true;
      }
    }
    return false;
  }
  bool setIndex(size_t index) {
    std::scoped_lock lock(queueMutex_);
    std::scoped_lock lock2(preloadedTracksMutex_);
    if (!position(index_, index))
      return false;
    reorderPreloadedLocked();
    return true;
  }

  bool setIndex(qconnect_QueueTrackRef& track) {
    return setIndex(track.queueItemId);
  }
  void addShuffleIndexes(size_t size = 0,
                         std::optional<size_t> insertAt = std::nullopt,
                         bool lock_queue = true) {
    if (lock_queue)
      std::scoped_lock lock(queueMutex_);
    if (!size)
      size = queue_.size();
    if (insertAt) {
      if (*insertAt < shuffledIndexes_.size())
        *insertAt = shuffledIndexes_[*insertAt];
      for (auto& i : shuffledIndexes_) {
        if (i >= *insertAt)
          i += size;
      }
      for (size_t i = 0; i < size; i++) {
        shuffledIndexes_.insert(shuffledIndexes_.begin() + *insertAt + i,
                                *insertAt + i);
      }
      return;
    }
    if (size) {
      for (size_t i = shuffledIndexes_.size(); i < size; i++) {
        shuffledIndexes_.push_back(i);
      }
      return;
    }
  }

  void shuffleIndexes(int32_t pivotQueueItemId = -1) {
    std::scoped_lock lock(queueMutex_);
    if (shuffledIndexes_.size() != queue_.size())
      addShuffleIndexes(0, std::nullopt, false);
    streamcore::esp_random_engine re;
    std::shuffle(shuffledIndexes_.begin(), shuffledIndexes_.end(), re);
    if (pivotQueueItemId >= 0) {
      auto it = std::find(shuffledIndexes_.begin(), shuffledIndexes_.end(),
                          pivotQueueItemId);
      if (it != shuffledIndexes_.end()) {
        std::iter_swap(shuffledIndexes_.begin(), it);
      }
    }
  }

  void clear() {
    std::scoped_lock lock(queueMutex_);
    std::scoped_lock lock2(preloadedTracksMutex_);
    deleteQobuzTracks();
    expandedTrackInfo_cache_.clear();
    index_ = 0;
  }

  void setStartAt(size_t startAtMs) {
    if (preloadedTracks_.size())
      preloadedTracks_.front()->startMs = startAtMs;
  }
  size_t getIndex() { return queue_.size() ? index_ : 0; }
  size_t getTrackIndex() {
    return queue_.size() ? queue_[index_].queueItemId : 0;
  }
  void setRepeat(bool repeat) {
    if (index_ < shuffledIndexes_.size()) {
      if (repeat) {
        if (index_ + preloadedTracks_.size() >= shuffledIndexes_.size()) {
          std::scoped_lock lock(queueMutex_);
          std::scoped_lock lock2(preloadedTracksMutex_);
          while (preloadedTracks_.size() + index_ >= shuffledIndexes_.size()) {
            preloadedTracks_.pop_back();
          }
          wantRestart_.store(true);
        }
      } else
        wantRestart_.store(false);
    }
  }
  void runTask() override;
  bool getFileUrl(std::shared_ptr<QobuzQueueTrack> track);
  bool getSuggestions();
  void onWsMessage(OnWsMessage f) { on_ws_msg_ = std::move(f); }
  void onGet(OnQobuzGet f) { on_qobuz_get_ = std::move(f); }
  void onPost(OnQobuzPost f) { on_qobuz_post_ = std::move(f); }
  void sendCommingTracks();
  void consumeQueueState(_qconnect_SrvrCtrlQueueState& state) {
    {
      qconnect_QueueVersion queueVersion = queueuState.queueVersion;
      PB_MOVE_ASSIGN(qconnect_SrvrCtrlQueueState, queueuState, state);
      shuffledIndexes_.clear();
      if (!state.has_queueVersion) {
        queueuState.queueVersion = queueVersion;
      }
      if (queueuState.shuffledTrackIndexes_count) {
        for (size_t i = 0; i < queueuState.shuffledTrackIndexes_count; i++) {
          shuffledIndexes_.push_back(queueuState.shuffledTrackIndexes[i]);
        }
      }
      if (shuffledIndexes_.size() != queueuState.tracks_count)
        addShuffleIndexes(queueuState.tracks_count);
      {
        std::scoped_lock lock(preloadedTracksMutex_);
        preloadedTracks_.clear();
      }
      if (queue_.size())
        return;
      addQobuzTracks(queueuState.tracks, queueuState.tracks_count);
      addQobuzTracks(queueuState.autoplayTracks,
                     queueuState.autoplayTracks_count);
    }
  }
  std::shared_ptr<QobuzQueueTrack> consumeTrack(
      std::shared_ptr<QobuzQueueTrack>, int32_t&);
  qconnect_SrvrCtrlQueueState queueuState =
      qconnect_SrvrCtrlQueueState_init_zero;

 private:
  std::atomic<bool> isRunning_;
  std::atomic<bool> wantRestart_{false};
  std::mutex isRunningMutex_;
  std::mutex queueMutex_;
  std::mutex preloadedTracksMutex_;
  std::deque<std::string> expandedTrackInfo_cache_;
  bool fetchedAutoplay_ = false;
  AudioFormat audioFormat_ = AudioFormat::QOBUZ_QUEUE_FORMAT_FLAC_LOSSLESS;

  size_t index_ = 0;
  size_t lastIndex_ = 0;
  uint8_t sessionId[16] = {0};
  std::vector<_qconnect_QueueTrackRef> queue_;
  std::vector<size_t> shuffledIndexes_;
  std::deque<std::shared_ptr<QobuzQueueTrack>> preloadedTracks_;

  bool getMetadata(std::shared_ptr<QobuzQueueTrack> track);
  bool loadMetadata(std::shared_ptr<QobuzQueueTrack> track,
                    nlohmann::json& json);
  bool getUrl(std::shared_ptr<QobuzQueueTrack> track);
  void reorderPreloadedLocked() {
    size_t i = 0;

    while (i < preloadedTracks_.size()) {
      // Compute desired queue index for logical position i
      size_t qidx = index_ + i;
      if (!shuffledIndexes_.empty() && qidx < shuffledIndexes_.size()) {
        if (qidx >= shuffledIndexes_.size()) {
          // Can't map further -> truncate tail
          preloadedTracks_.erase(preloadedTracks_.begin() + i,
                                 preloadedTracks_.end());
          break;
        }
        qidx = shuffledIndexes_[qidx];
      }
      const size_t wantId = queue_[qidx].queueItemId;

      // Search only the suffix [i, end) to preserve already-placed prefix order
      auto it =
          std::find_if(preloadedTracks_.begin() + i, preloadedTracks_.end(),
                       [&](const std::shared_ptr<QobuzQueueTrack>& t) {
                         return t->index == wantId;
                       });

      if (it == preloadedTracks_.end()) {
        // First gap -> drop everything after i
        preloadedTracks_.erase(preloadedTracks_.begin() + i,
                               preloadedTracks_.end());
        break;
      }

      // Put the found track into place i (swap is no-op if it == begin()+i)
      std::iter_swap(preloadedTracks_.begin() + i, it);
      ++i;
    }
  }
  OnWsMessage on_ws_msg_;
  OnQobuzGet on_qobuz_get_;
  OnQobuzPost on_qobuz_post_;
};

}  // namespace qobuz