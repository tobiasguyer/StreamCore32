#include <sstream>

#include "QobuzQueue.h"

#include "nlohmann/json.hpp"
#include "nlohmann/json_fwd.hpp"

#include "BellLogger.h"

namespace qobuz {
  bool QobuzQueue::getMetadata(std::shared_ptr<QobuzQueueTrack> track) {
    if (!track->id) return false;
    auto resp = on_qobuz_get_("track", "get", { {"track_id", std::to_string(track->id)} }, false);
    if (resp->status() == 200) {
      auto json = nlohmann::json::parse(resp->body_string());
      if (json.empty()) return false;
      return loadMetadata(track, json);
    }
    else {
      BELL_LOG(error, "queue", "QobuzQueue::gotMetadata: %s", resp->body_string().c_str());
    }
    return false;
  }

  bool QobuzQueue::loadMetadata(std::shared_ptr<QobuzQueueTrack> track, nlohmann::json& json) {
    if (json.empty()) return false;

    if (json.find("streamable") == json.end() || json["streamable"] == false) return false;
    if (track->format > AudioFormat::QOBUZ_QUEUE_FORMAT_FLAC_LOSSLESS) {
      if (json.find("hires_streamable") == json.end() || json["hires_streamable"] == false)
        track->format = AudioFormat::QOBUZ_QUEUE_FORMAT_FLAC_LOSSLESS;
      else {
        uint32_t maxSamplingRate = json.find("maximum_sampling_rate") == json.end() ? 44100
          : (uint32_t)(json["maximum_sampling_rate"].get<double>() * 1000);
        if (maxSamplingRate <= 44100) track->format = AudioFormat::QOBUZ_QUEUE_FORMAT_FLAC_LOSSLESS;
        else if (maxSamplingRate <= 96000) track->format = AudioFormat::QOBUZ_QUEUE_FORMAT_FLAC_HI_RES_96;
      }
    }
    track->durationMs = json.at("duration").get<uint32_t>() * 1000;
    track->n_channels = json.at("maximum_channel_count").get<int>();

    track->title = json["title"];
    if (json.find("performer") != json.end()) {
      track->artist.id = json["performer"]["id"];
      track->artist.name = json["performer"]["name"];
    }

    if (json.find("album") != json.end()) {
      track->album.id = json["album"]["id"];
      track->album.name = json["album"]["title"];
      track->album.qobuz_id = json["album"]["qobuz_id"];
      track->album.url = json["album"]["url"];
      if (json["album"].find("image") != json["album"].end()) {
        if (json["album"]["image"].find("large") != json["album"]["image"].end()) track->album.image.large_img = json["album"]["image"]["large"];
        if (json["album"]["image"].find("small") != json["album"]["image"].end()) track->album.image.small_img = json["album"]["image"]["small"];
        if (json["album"]["image"].find("thumbnail") != json["album"]["image"].end()) track->album.image.thumbnail = json["album"]["image"]["thumbnail"];
      }
      if (json["album"].find("genre") != json["album"].end()) {
        if (json["album"]["genre"].find("id") != json["album"]["genre"].end())
          track->album.genre_id = json["album"]["genre"]["id"];
      }
      if (json["album"].find("label") != json["album"].end())
        track->album.label_id = json["album"]["label"]["id"];
    }

    track->state = QueuedTrackState::STREAMABLE;
    return true;
  }

  bool QobuzQueue::getFileUrl(std::shared_ptr<QobuzQueueTrack> track) {
    if (!track->id) return false;
    auto resp = on_qobuz_get_("track", "getFileUrl",
      {
        {"format_id", std::to_string(track->format)},
        {"intent", "stream"},
        {"track_id", std::to_string(track->id)}
      }, true);
    if (resp->status() == 200) {
      auto json = nlohmann::json::parse(resp->body_string());
      if (json.empty()) return false;
      if (json.contains("status") && json["status"] == "error") return false;
      if (json.contains("url")) track->fileUrl = json["url"].get<std::string>();
      if (json.contains("blob")) track->blob = json["blob"].get<std::string>();
      if (json.contains("duration")) track->durationMs = (size_t)(json["duration"].get<size_t>() * 1000);
      if (json.contains("n_channels")) track->n_channels = json["n_channels"].get<int>();
      if (json.contains("bit_depth")) track->bits_depth = json["bit_depth"].get<int>();
      if (json.contains("sampling_rate")) track->sampling_rate = (int)(json["sampling_rate"].get<double>() * 1000);
      BELL_LOG(info, "queue", "QobuzQueue::getFileUrl: ms=%lu, channels=%d, depth=%d, rate=%d", track->durationMs, track->n_channels, track->bits_depth, track->sampling_rate);
      track->state = QueuedTrackState::READY;
      return true;
    }
    else {
      BELL_LOG(error, "queue", "QobuzQueue::getFileUrl: %s", resp->body_string().c_str());
    }
    return false;
  }

  static std::string buildSuggestionsPayload(std::vector<qconnect_QueueTrackRef>& tracks, std::deque<std::string>& expandedTrackCache, uint64_t limit = 20) {
    std::ostringstream oss;
    oss << "{"
      << "\"limit\":" << limit << ","
      << "\"listened_tracks_ids\":[";
    for (int i = tracks.size() < 100 ? 0 : tracks.size() - 100; i < tracks.size(); i++) {
      oss << tracks[i].trackId;
      if (i < tracks.size() - 1) oss << ",";
    }
    oss << "]," << "\"track_to_analysed\":[";
    if (expandedTrackCache.size())
      for (int i = 0; i < expandedTrackCache.size(); i++) {
        oss << expandedTrackCache[i];
        if (i < expandedTrackCache.size() - 1) oss << ",";
      }
    oss << "]"
      << "}";
    return oss.str();
  }
uint8_t HDigit2Dez(const char c) {
  if (c >= 'A') return 10 + c - 'A';
  return c - '0';
}

uint8_t H2Digit2Dez(const char *arr) {
  return HDigit2Dez(arr[0]) * 16 + HDigit2Dez(arr[1]);
}
  bool QobuzQueue::getSuggestions() {
    size_t currentSize = queue_.size();
    if (!currentSize) return false;
    size_t lastTrackQId = queue_.back().queueItemId;
    while(1){
    auto resp = on_qobuz_post_(
      "dynamic", "suggest", buildSuggestionsPayload(queue_, expandedTrackInfo_cache_),
      {}, false);
    if (resp->status() == 200) {
      if (resp->contentLength() == 0) return false;
      auto json = nlohmann::json::parse(resp->body_string());
      if (json.empty()) return false;
      if (json.find("tracks") != json.end()) {
        nlohmann::json& tracks = json["tracks"]["items"];

        qconnect_QConnectMessage msg = qconnect_QConnectMessage_init_zero;
        msg.has_messageType = true;
        msg.messageType = qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_AUTOPLAY_ADD_TRACKS;
        msg.has_ctrlSrvrAutoplayLoadTracks = true;
        if (!tracks.size()) return false;
        msg.ctrlSrvrAutoplayLoadTracks.trackIds_count = tracks.size() + 1;
        msg.ctrlSrvrAutoplayLoadTracks.trackIds = (uint32_t*)calloc(msg.ctrlSrvrAutoplayLoadTracks.trackIds_count, sizeof(uint32_t));
        
          msg.ctrlSrvrAutoplayLoadTracks.trackIds[0] = preloadedTracks_.back()->id;
        
        for (int i = 0; i < tracks.size(); i++) {
          msg.ctrlSrvrAutoplayLoadTracks.trackIds[i + 1] = tracks[i]["id"];
        }
        msg.ctrlSrvrAutoplayLoadTracks.has_queueVersion = true;
        msg.ctrlSrvrAutoplayLoadTracks.queueVersion = queueuState.queueVersion;
        if(resp->header("set-cookie").size()) {
          std::string cookie = std::string(resp->header("set-cookie"));
          if(cookie.find("qobuz-session") != std::string::npos) {
            std::string session = cookie.substr(cookie.find("qobuz-session=") + 14, cookie.find(";") - (cookie.find("qobuz-session=") + 14));
            std::vector<uint8_t> session_to_16;
            for (int i = 0; i < session.size(); i+=2) {
              session_to_16.push_back(H2Digit2Dez(session.substr(i, 2).c_str()));
            }
            msg.ctrlSrvrAutoplayLoadTracks.contextUuid = vectorToPbArray(session_to_16);
          }
        } else  msg.ctrlSrvrAutoplayLoadTracks.contextUuid = dataToPbArray(sessionId, sizeof(sessionId));
        msg.ctrlSrvrAutoplayLoadTracks.actionUuid = vectorToPbArray(pbArrayToVector(queueuState.actionUuid));

        on_ws_msg_(&msg, 1);
        return true;
      }
    } else if(resp->status() <= 400) {
      BELL_LOG(error, "queue", "QobuzQueue::getSuggestions: %s", resp->body_string().c_str());
      continue;
    }
    else {
      BELL_LOG(error, "queue", "QobuzQueue::getSuggestions: %s", resp->body_string().c_str());
    }
    return false;
  }
  }
  void QobuzQueue::runTask() {
    isRunning_.store(true);
    std::scoped_lock lock(isRunningMutex_);
    std::deque<std::shared_ptr<QobuzQueueTrack>> tracks;
    while (isRunning_) {
      if (!queue_.size()) {
        BELL_SLEEP_MS(50);
        continue;
      }
      if (preloadedTracks_.size() < 3 && !fetchedAutoplay_) {
        std::scoped_lock lock(queueMutex_);
        std::scoped_lock lock2(preloadedTracksMutex_);
        for (int i = preloadedTracks_.size(); i < 3; i++) {
          if (index_ + preloadedTracks_.size() < queue_.size()) {
            size_t idx = index_ + preloadedTracks_.size();
            if (!shuffledIndexes_.empty() && idx < shuffledIndexes_.size()) {
              idx = shuffledIndexes_[idx];
            }
            else if (wantRestart_.load()) {
              index_ = 0;
              break;
            }
            preloadedTracks_.push_back(std::make_shared<QobuzQueueTrack>(queue_[idx]));
          } else break;
        }
        if(!fetchedAutoplay_ && preloadedTracks_.size() < 2 && expandedTrackInfo_cache_.size()) {
            if (!getSuggestions()) {
              isRunning_.store(false);
              break;
            }
            fetchedAutoplay_ = true;
          }
      } else BELL_SLEEP_MS(50);
      if (!preloadedTracks_.size()) BELL_SLEEP_MS(300);
      {
        std::scoped_lock lock(preloadedTracksMutex_);
        tracks = preloadedTracks_;
      }

      for (auto& track : tracks) {
        bool done = false;
        switch (track->state) {
        case QueuedTrackState::QUEUED:
          track->state = QueuedTrackState::PENDING_META;
          if (!getMetadata(track)) track->state = QueuedTrackState::FAILED;
          else done = true;
          break;
        case QueuedTrackState::STREAMABLE:
          track->state = QueuedTrackState::PENDING_FILE;
          if (!getFileUrl(track)) track->state = QueuedTrackState::FAILED;
          else {
            std::string ctx = track->contextJson();
            {
              expandedTrackInfo_cache_.push_back(ctx);
              if (expandedTrackInfo_cache_.size() > 5) expandedTrackInfo_cache_.pop_front();
            }
          }
          break;
        default:
          if (track == tracks.back()) {
            BELL_SLEEP_MS(300); // wait for next track to load
          
          }
          break;
        }
        BELL_SLEEP_MS(1);
        if (done) break;
      }
    }
    isRunning_.store(false);
  }

  std::shared_ptr<QobuzQueueTrack> QobuzQueue::consumeTrack(std::shared_ptr<QobuzQueueTrack> prevTrack, int32_t& nextTrackQueueIndex) {
    if (!queue_.size()) return nullptr;
    while (true) {
      if(fetchedAutoplay_) {
        BELL_SLEEP_MS(100);
        continue;
      }
      if (preloadedTracks_.empty()) {
        BELL_SLEEP_MS(100);
        if(index_ >= queue_.size()) {
          return nullptr;
        }
        continue;
      }
      std::shared_ptr<qobuz::QobuzQueueTrack> track;
      {
        std::scoped_lock lock(preloadedTracksMutex_);
        if(prevTrack) {
          auto prevTrackIter = std::find(preloadedTracks_.begin(), preloadedTracks_.end(), prevTrack);
          if (prevTrackIter != preloadedTracks_.end()) {
            lastIndex_ = index_;
            index_++;
            preloadedTracks_.erase(prevTrackIter);
          }
        }
        track = preloadedTracks_.front();
        if (preloadedTracks_.size() > 1) nextTrackQueueIndex = preloadedTracks_[1]->index;
        else nextTrackQueueIndex = 0;
        if(index_ >= shuffledIndexes_.size()) {
          shuffledIndexes_.push_back(index_);
        }
      }
      if (track->state != QueuedTrackState::READY && track->state != QueuedTrackState::FAILED) {
        BELL_SLEEP_MS(100);
        continue;
      }
      return track;
    }
  }
}