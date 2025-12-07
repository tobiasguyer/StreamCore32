#include "TrackQueue.h"
#include <pb_decode.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <random>

#include "AccessKeyFetcher.h"
#include "BellTask.h"
#include "BellUtils.h"  // for BELL_SLEEP_MS
#include "CDNAudioFile.h"
#include "HTTPClient.h"
#include "Logger.h"
#include "SpotifyContext.h"
#include "Utils.h"
#include "WrappedSemaphore.h"
#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif
#include "protobuf/metadata.pb.h"

using namespace spotify;
static bell::WrappedSemaphore* processSemaphore_ = nullptr;
namespace TrackDataUtils {
bool countryListContains(char* countryList, const char* country) {
  uint16_t countryList_length = strlen(countryList);
  for (int x = 0; x < countryList_length; x += 2) {
    if (countryList[x] == country[0] && countryList[x + 1] == country[1]) {
      return true;
    }
  }
  return false;
}

bool doRestrictionsApply(Restriction* restrictions, int count,
                         const char* country) {
  for (int x = 0; x < count; x++) {
    if (restrictions[x].countries_allowed != nullptr) {
      return !countryListContains(restrictions[x].countries_allowed, country);
    }

    if (restrictions[x].countries_forbidden != nullptr) {
      return countryListContains(restrictions[x].countries_forbidden, country);
    }
  }

  return false;
}

bool canPlayTrack(Track& trackInfo, int altIndex, const char* country) {
  if (altIndex < 0) {

  } else {
    for (int x = 0; x < trackInfo.alternative[altIndex].restriction_count;
         x++) {
      if (trackInfo.alternative[altIndex].restriction[x].countries_allowed !=
          nullptr) {
        return countryListContains(
            trackInfo.alternative[altIndex].restriction[x].countries_allowed,
            country);
      }

      if (trackInfo.alternative[altIndex].restriction[x].countries_forbidden !=
          nullptr) {
        return !countryListContains(
            trackInfo.alternative[altIndex].restriction[x].countries_forbidden,
            country);
      }
    }
  }
  return true;
}
}  // namespace TrackDataUtils

void TrackInfo::loadPbTrack(Track* pbTrack, const std::vector<uint8_t>& gid) {
  // Generate ID based on GID
  trackId = bytesToHexString(gid);

  name = std::string(pbTrack->name);

  if (pbTrack->artist_count > 0) {
    // Handle artist data
    artist = std::string(pbTrack->artist[0].name);
  }

  if (pbTrack->has_album) {
    // Handle album data
    album = std::string(pbTrack->album.name);

    if (pbTrack->album.has_cover_group &&
        pbTrack->album.cover_group.image_count > 0) {
      auto imageId =
          pbArrayToVector(pbTrack->album.cover_group
                              .image[pbTrack->album.cover_group.image_count - 1]
                              .file_id);
      imageUrl = "https://i.scdn.co/image/" + bytesToHexString(imageId);
    }
  }

  number = pbTrack->has_number ? pbTrack->number : 0;
  discNumber = pbTrack->has_disc_number ? pbTrack->disc_number : 0;
  duration = pbTrack->duration;
}

void TrackInfo::loadPbEpisode(Episode* pbEpisode,
                              const std::vector<uint8_t>& gid) {
  // Generate ID based on GID
  trackId = bytesToHexString(gid);

  name = std::string(pbEpisode->name);

  if (pbEpisode->covers.image_count > 0) {
    // Handle episode info
    auto imageId = pbArrayToVector(
        pbEpisode->covers.image[pbEpisode->covers.image_count - 1].file_id);
    imageUrl = "https://i.scdn.co/image/" + bytesToHexString(imageId);
  }

  number = pbEpisode->has_number ? pbEpisode->number : 0;
  discNumber = 0;
  duration = pbEpisode->duration;
}

QueuedTrack::QueuedTrack(
    player_proto_connect_ProvidedTrack& ref,
    std::shared_ptr<spotify::Context> ctx,
    std::shared_ptr<bell::WrappedSemaphore> playableSemaphore,
    int64_t requestedPosition)
    : requestedPosition((uint32_t)requestedPosition), ctx(ctx) {
  trackMetrics = std::make_shared<TrackMetrics>(ctx, requestedPosition);
  this->playableSemaphore = playableSemaphore;
  this->ref = ref;
  this->audioFormat = ctx->config.audioFormat;
  this->trackInfo.provider = ref.provider;
  for (int i = 0; i < ref.full_metadata_count; i++) {
    if (strcmp(ref.metadata[i].key, "page_instance_id") == 0) {
      this->trackInfo.page_instance_id = ref.metadata[i].value;
    } else if (strcmp(ref.metadata[i].key, "interaction_id") == 0) {
      this->trackInfo.interaction_id = ref.metadata[i].value;
    } else if (strcmp(ref.metadata[i].key, "decision_id") == 0) {
      this->trackInfo.decision_id = ref.metadata[i].value;
    }
  }
  if (!strstr(ref.uri, "spotify:delimiter")) {
    this->gid = base62Decode(ref.uri);
    state = State::QUEUED;
    processSemaphore_->give();
  } else {
    cancelLoading();
  }
}

QueuedTrack::~QueuedTrack() {
  //if (state < State::READY)
  //  playableSemaphore->give();
  if (state < State::READY)
    state = State::FAILED;

  if (pendingMercuryRequest != 0) {
    ctx->session->unregister(pendingMercuryRequest);
  }

  if (pendingAudioKeyRequest != 0) {
    ctx->session->unregisterAudioKey(pendingAudioKeyRequest);
  }
  if (trackMetrics->audioKeyTime) {
    trackMetrics->endTrack();
    ctx->playbackMetrics->sendEvent(this);
  }
  pb_release(Track_fields, &pbTrack);
  pb_release(Episode_fields, &pbEpisode);
}

void QueuedTrack::cancelLoading() {
  state = State::FAILED;
  playableSemaphore->give();
  processSemaphore_->give();
}

std::shared_ptr<spotify::CDNAudioFile> QueuedTrack::getAudioFile() {
  if (state != State::READY) {
    return nullptr;
  }

  return std::make_shared<spotify::CDNAudioFile>(cdnUrl, audioKey);
}

bool QueuedTrack::stepParseMetadata(Track* pbTrack, Episode* pbEpisode) {
  int alternativeCount, filesCount = 0;
  bool canPlay = false;
  AudioFile* selectedFiles = nullptr;

  const char* countryCode = ctx->session->getCountryCode().c_str();

  if (gid.first == SpotifyFileType::TRACK) {

    // Check if we can play the track, if not, try alternatives
    if (TrackDataUtils::doRestrictionsApply(
            pbTrack->restriction, pbTrack->restriction_count, countryCode)) {
      // Go through alternatives
      for (int x = 0; x < pbTrack->alternative_count; x++) {
        if (!TrackDataUtils::doRestrictionsApply(
                pbTrack->alternative[x].restriction,
                pbTrack->alternative[x].restriction_count, countryCode)) {
          SC32_LOG(info, "Found alternative track");
          selectedFiles = pbTrack->alternative[x].file;
          filesCount = pbTrack->alternative[x].file_count;
          trackId = pbArrayToVector(pbTrack->alternative[x].gid);
          break;
        }
      }
    } else {
      // We can play the track
      selectedFiles = pbTrack->file;
      filesCount = pbTrack->file_count;
      trackId = pbArrayToVector(pbTrack->gid);
    }

    if (trackId.size() > 0) {
      // Load track information
      trackInfo.loadPbTrack(pbTrack, trackId);
    }
  } else {
    // Handle episodes

    // Check if we can play the episode
    if (!TrackDataUtils::doRestrictionsApply(pbEpisode->restriction,
                                             pbEpisode->restriction_count,
                                             countryCode)) {
      selectedFiles = pbEpisode->file;
      filesCount = pbEpisode->file_count;
      trackId = pbArrayToVector(pbEpisode->gid);

      // Load track information
      trackInfo.loadPbEpisode(pbEpisode, trackId);
    }
  }
  if (selectedFiles == nullptr) {
    SC32_LOG(info, "No playable files found");
    return false;
  }
  // Find playable file
  for (int x = 0; x < filesCount; x++) {
    if (selectedFiles[x].format == audioFormat) {
      fileId = pbArrayToVector(selectedFiles[x].file_id);
      break;  // If file found stop searching
    }

    // Fallback to OGG Vorbis 96kbps
    if (fileId.size() == 0 &&
        selectedFiles[x].format == AudioFormat_OGG_VORBIS_96) {
      fileId = pbArrayToVector(selectedFiles[x].file_id);
      SC32_LOG(info, "Falling back to OGG Vorbis 96kbps");
    }
  }

  // No viable files found for playback
  if (fileId.size() == 0) {
    SC32_LOG(info, "File not available for playback");
    return false;
  }
  identifier = bytesToHexString(fileId);
  state = State::KEY_REQUIRED;
  return true;
}

void QueuedTrack::stepLoadAudioFile(
    std::mutex& trackListMutex,
    std::shared_ptr<bell::WrappedSemaphore> updateSemaphore) {
  // Request audio key
  this->pendingAudioKeyRequest = ctx->session->requestAudioKey(
      trackId, fileId,
      [this, &trackListMutex, updateSemaphore](
          bool success, const std::vector<uint8_t>& audioKey) {
        std::scoped_lock lock(trackListMutex);

        if (success) {
          this->audioKey =
              std::vector<uint8_t>(audioKey.begin() + 4, audioKey.end());

          state = State::CDN_REQUIRED;
          updateSemaphore->give();
        } else {
          SC32_LOG(error, "Failed to get audio key");
          retries++;
          state = State::KEY_REQUIRED;
          if (retries > 10) {
            if (audioFormat > AudioFormat_OGG_VORBIS_96) {
              audioFormat = (AudioFormat)(audioFormat - 1);
              state = State::QUEUED;
              updateSemaphore->give();
            } else {
              cancelLoading();
            }
          }
        }
      });

  state = State::PENDING_KEY;
}

void QueuedTrack::stepLoadCDNUrl(const std::string& accessKey) {
  if (accessKey.size() == 0) {
    // Wait for access key
    return;
  }

  // Request CDN URL

  try {

    std::string requestUrl = string_format(
        "https://api.spotify.com/v1/storage-resolve/files/audio/interactive/"
        "%s?alt=json",
        bytesToHexString(fileId).c_str());
    auto req = bell::HTTPClient::get(
        requestUrl, {bell::HTTPClient::ValueHeader(
                        {"Authorization", "Bearer " + accessKey})});

    // Wait for response
    std::string result = req->body_string();
    if (result == "") {
      state = State::FAILED;
      playableSemaphore->give();
      return;
    }
#ifdef BELL_ONLY_CJSON
    cJSON* jsonResult = cJSON_Parse(result.data());
    cdnUrl = cJSON_GetArrayItem(cJSON_GetObjectItem(jsonResult, "cdnurl"), 0)
                 ->valuestring;
    cJSON_Delete(jsonResult);
#else
    auto jsonResult = nlohmann::json::parse(result);
    cdnUrl = jsonResult["cdnurl"][0];
#endif

    // SC32_LOG(info, "Received CDN URL, %s", cdnUrl.c_str());
    state = State::READY;
  } catch (...) {
    SC32_LOG(error, "Cannot fetch CDN URL");
    state = State::FAILED;
  }
  playableSemaphore->give();
}

void QueuedTrack::stepLoadMetadata(
    Track* pbTrack, Episode* pbEpisode, std::mutex& trackListMutex,
    std::shared_ptr<bell::WrappedSemaphore> updateSemaphore) {
  // Prepare request ID
  std::string requestUrl =
      string_format("hm://metadata/3/%s/%s",
                    gid.first == SpotifyFileType::TRACK ? "track" : "episode",
                    bytesToHexString(gid.second).c_str());

  auto responseHandler = [this, pbTrack, pbEpisode, &trackListMutex,
                          updateSemaphore](MercurySession::Response res) {
    std::scoped_lock lock(trackListMutex);

    if (res.parts.size() == 0) {
      SC32_LOG(info, "Invalid Metadata");
      // Invalid metadata, cannot proceed
      cancelLoading();
      return;
    }
    bool ret = false;
    if (gid.first == SpotifyFileType::TRACK) {
      pb_release(Track_fields, pbTrack);
      ret = pbDecode(*pbTrack, Track_fields, res.parts[0]);
    } else {
      pb_release(Episode_fields, pbEpisode);
      ret = pbDecode(*pbEpisode, Episode_fields, res.parts[0]);
    }
    if (!ret) {
      SC32_LOG(info, "Failed to decode Metadata");
      cancelLoading();
      return;
    }
    // Parse received metadata
    ret = stepParseMetadata(pbTrack, pbEpisode);
    if (!ret) {
      SC32_LOG(info, "Failed to parse Metadata");
      cancelLoading();
      return;
    }
    updateSemaphore->give();
  };
  // Execute the request
  if (pbTrack != NULL || pbEpisode != NULL)
    pendingMercuryRequest = ctx->session->execute(
        MercurySession::RequestType::GET, requestUrl, responseHandler);
  else {
    SC32_LOG(info, "Invalid Metadata");
    // Invalid metadata, cannot proceed
    cancelLoading();
    return;
  }

  // Set the state to pending
  state = State::PENDING_META;
}

TrackQueue::TrackQueue(std::shared_ptr<spotify::Context> ctx)
    : bell::Task("spotify_queue", 1024 * 12, 0, 1), ctx(ctx) {
  accessKeyFetcher = std::make_shared<spotify::AccessKeyFetcher>(ctx);
  processSemaphore = std::make_shared<bell::WrappedSemaphore>();
  processSemaphore_ =
      static_cast<bell::WrappedSemaphore*>(processSemaphore.get());
  playableSemaphore = std::make_shared<bell::WrappedSemaphore>();

  // Start the task
  startTask();
};

TrackQueue::~TrackQueue() {
  stopTask();

  std::scoped_lock lock(tracksMutex);
}

void TrackQueue::runTask() {
  isRunning = true;

  std::scoped_lock lock(runningMutex);

  std::deque<std::shared_ptr<QueuedTrack>> trackQueue;

  while (isRunning) {
    if (processSemaphore->twait(200)) {
      continue;
    }

    // Make sure we have the newest access key
    accessKey = accessKeyFetcher->getAccessKey();
    {
      std::scoped_lock lock(tracksMutex);

      trackQueue = preloadedTracks;
    }

    for (auto& track : trackQueue) {
      if (track) {
        if (this->processTrack(track))
          break;
      }
    }
  }
}

void TrackQueue::stopTask() {
  if (isRunning) {
    isRunning = false;
    std::scoped_lock lock(runningMutex);
  }
}

std::shared_ptr<QueuedTrack> TrackQueue::consumeTrack(
    std::shared_ptr<QueuedTrack> prevTrack, int& offset) {
  std::scoped_lock lock(tracksMutex);

  if (!preloadedTracks.size()) {
    offset = -1;
    return nullptr;
  }

  auto prevTrackIter =
      std::find(preloadedTracks.begin(), preloadedTracks.end(), prevTrack);

  if (prevTrackIter != preloadedTracks.end()) {
    // Get offset of next track
    offset = prevTrackIter - preloadedTracks.begin() + 1;
  } else {
    offset = 0;
  }
  if (offset >= preloadedTracks.size()) {
    // Last track in preloaded queue
    return nullptr;
  }
  return preloadedTracks[offset];
}

bool TrackQueue::processTrack(std::shared_ptr<QueuedTrack> track) {
  switch (track->state) {
    case QueuedTrack::State::QUEUED:
      track->stepLoadMetadata(&track->pbTrack, &track->pbEpisode, tracksMutex,
                              processSemaphore);
      break;
    case QueuedTrack::State::KEY_REQUIRED:
      track->stepLoadAudioFile(tracksMutex, processSemaphore);
      break;
    case QueuedTrack::State::CDN_REQUIRED:
      track->stepLoadCDNUrl(accessKey);
      break;
    default:
      return false;
      // Do not perform any action
      break;
  }
  return true;
}
