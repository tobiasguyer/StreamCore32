#include "DeviceStateHandler.h"

#include <string.h>  // for strdup, memcpy, strcpy, strlen
#include <cstdint>   // for uint8_t
#include <cstdlib>   // for unreference, NULL, realloc, rand
#include <cstring>
#include <memory>       // for shared_ptr
#include <type_traits>  // for remove_extent_t
#include <utility>      // for swap

#include "BellLogger.h"           // for AbstractLogger
#include "BellUtils.h"            // for BELL_SLEEP_MS
#include "SpotifyContext.h"         // for Context::ConfigState, Context (ptr o...
#include "ConstantParameters.h"   // for protocolVersion, swVersion
#include "Logger.h"               // for SC32_LOG
#include "NanoPBHelper.h"         // for pbEncode, pbPutString
#include "Packet.h"               // for spotify
#include "TrackReference.h"       // for spotify
#include "WrappedSemaphore.h"     // for WrappedSemaphore
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#include "pb.h"                   // for pb_bytes_array_t, PB_BYTES_ARRAY_T_A...
#include "pb_decode.h"            // for pb_release
#include "protobuf/transfer_state.pb.h"  // for player_proto_connect_PutStateRequest

using namespace spotify;

#if defined(_WIN32) || defined(_WIN64)
char* strndup(const char* str, size_t n) {
  /**
   * Allocate and copy a given number of characters from a given string.
   *
   * This is a Windows-compatible version of POSIX's strndup, which is not
   * available on Windows. It is necessary because the Windows SDK does not
   * provide this function.
   *
   * @param str The string to copy from.
   * @param n   The number of characters to copy.
   *
   * @return The copied string, or null if memory allocation fails.
   */
  if (!str)
    return nullptr;  // Handle null input gracefully
  size_t len = std::strlen(str);
  if (len > n)
    len = n;                                 // Limit to n characters
  char* copy = (char*)std::malloc(len + 1);  // Allocate memory
  if (!copy)
    return nullptr;             // Return null if allocation fails
  std::memcpy(copy, str, len);  // Copy the characters
  copy[len] = '\0';             // Null-terminate the string
  return copy;
}
#endif

void remove_tracks_by_provider(std::vector<player_proto_connect_ProvidedTrack>& v, const char* provider, size_t offset, bool is_provided_by = false) {
  if (offset >= v.size()) return;
  size_t w = offset; // write index
  for (size_t r = offset; r < v.size(); ++r) {
    if (std::strcmp(v[r].provider, provider) == is_provided_by) {
      if (w != r) v[w] = v[r];
      ++w;
    }
    else {
      spotify::TrackReference::pbReleaseProvidedTrack(&v[r]); //
    }
  }
  if (w < v.size()) v.resize(w);

}

template <typename Entry>
static inline char*
pb_map_get_value(const Entry* md, std::size_t md_count, const char* key)
{
  if (!md || !key) return nullptr;
  for (std::size_t i = 0; i < md_count; ++i) {
    const char* k = md[i].key;
    if (k && std::strcmp(k, key) == 0) {
      return md[i].value;
    }
  }
  return nullptr;
}
static inline bool
move_ContextTrack_to_ProvidedTrack(player_proto_ContextTrack* src,
  player_proto_connect_ProvidedTrack* dst)
{
  if (!src || !dst) return false;

  *dst = player_proto_connect_ProvidedTrack_init_zero;

  dst->uri = src->uri;   src->uri = NULL;
  dst->uid = src->uid;   src->uid = NULL;

  if (!dst->uri || dst->uri[0] == '\0') {
    if (dst->uri) { free(dst->uri); dst->uri = NULL; }
    if (src->gid) {
      std::vector<uint8_t> gidBytes = pbArrayToVector(src->gid);
      if (!gidBytes.empty()) {
        std::string uri = base62EncodeUri({ SpotifyFileType::TRACK, gidBytes });
        dst->uri = strdup(uri.c_str());
        if (!dst->uri) return false; // OOM
      }
    }
  }
  dst->gid = src->gid; src->gid = NULL;

  dst->provider = strdup("connect");
  if (!dst->provider) {
    return false;
  }

  dst->metadata = NULL;
  dst->metadata_count = 0;
  dst->full_metadata_count = 0;
  dst->has_full_metadata_count = false;

  const size_t n = src->metadata_count;
  if (n > 0 && src->metadata) {
    using DstEntry = MetadataEntry;
    dst->metadata = (DstEntry*)calloc(n, sizeof(DstEntry));
    if (!dst->metadata) {
      return false;
    }

    for (size_t i = 0; i < n; ++i) {
      dst->metadata[i].key = src->metadata[i].key;
      dst->metadata[i].value = src->metadata[i].value;
      src->metadata[i].key = NULL;
      src->metadata[i].value = NULL;
    }

    dst->metadata_count = n;
    dst->full_metadata_count = static_cast<int32_t>(n);
    dst->has_full_metadata_count = true;

    free(src->metadata);
    src->metadata = NULL;
    src->metadata_count = 0;
  }
  return true;
}

static DeviceStateHandler* handler;

DeviceStateHandler::DeviceStateHandler(std::shared_ptr<spotify::LoginBlob> blob,
  std::function<void(bool)> onClose, std::function<void()> onTransfer,
  std::function<void(const std::string& username,
    const std::vector<uint8_t>& auth_data)> onLogin,
  std::function<uint16_t()> volume)
  : bell::Task("spotify_state_handler", 8 * 1024, 0, 1) {
  handler = this;
  this->ctx = spotify::Context::createFromBlob(blob);
  this->ctx->config.volume = volume;
  auto connectStateSubscription = [this](MercurySession::Response res) {
    if (res.fail || !res.parts.size())
      return;
    if (strstr(res.mercuryHeader.uri, "v1/devices/")) {
      putDeviceState(player_proto_connect_PutStateReason::player_proto_connect_PutStateReason_SPIRC_NOTIFY);
    }
    else if (strstr(res.mercuryHeader.uri, "player/command")) {
      if (res.parts[0].size())
        parseCommand(res.parts[0]);
    }
    else if (strstr(res.mercuryHeader.uri, "volume")) {
      if (res.parts[0].size()) {
        player_proto_connect_SetVolumeCommand newVolume;
        pbDecode(newVolume, player_proto_connect_SetVolumeCommand_fields, res.parts[0]);
        device.device_info.volume = newVolume.volume;
        device.device_info.has_volume = true;
        sinkCommand(CommandType::VOLUME, newVolume.volume);
        putDeviceState(player_proto_connect_PutStateReason::player_proto_connect_PutStateReason_PLAYER_STATE_CHANGED);
        pb_release(player_proto_connect_SetVolumeCommand_fields, &newVolume);
      }
    }
    else if (strstr(res.mercuryHeader.uri, "cluster")) {
      if (0) {  // will send cluster info if new device logged in, but connect_state/cluster is called way too often(for example during each song) too send each time a putPlayerStateRequest
        //if(is_active){
        putPlayerState();
      }
    }
    else if (strstr(res.mercuryHeader.uri, "v1/connect/logout")) {
      disconnect(true);
    }
    else
      SC32_LOG(debug, "Unknown connect_state, uri : %s",
        res.mercuryHeader.uri);
    };

  this->ctx->session->addSubscriptionListener("hm://connect-state/",
    connectStateSubscription);
  SC32_LOG(info, "Added connect-state subscription");

  // the device connection status gets reported trough "hm://social-connect",if active
  auto socialConnectSubscription = [this](MercurySession::Response res) {
    if (res.fail || !res.parts.size())
      return;
    if (res.parts[0].size()) {
      auto jsonResult = nlohmann::json::parse(res.parts[0]);
      if (jsonResult.find("deviceBroadcastStatus") != jsonResult.end()) {
        if (jsonResult.find("deviceBroadcastStatus")->find("device_id") !=
          jsonResult.find("deviceBroadcastStatus")->end()) {
          if (jsonResult.find("deviceBroadcastStatus")
            ->at("device_id")
            .get<std::string>() != this->ctx->config.deviceId)
            goto changePlayerState;
        }
      }
      else if (jsonResult.find("reason") != jsonResult.end() &&
        jsonResult.at("reason") == "SESSION_DELETED")
        goto changePlayerState;
      return;
    changePlayerState:
      if (this->is_active) {
        this->ctx->playbackMetrics->end_reason = PlaybackMetrics::REMOTE;
        this->ctx->playbackMetrics->end_source = "unknown";
        this->trackPlayer->stop();
        this->is_active = false;
        if (device.player_state.has_restrictions)
          pb_release(player_proto_Restrictions_fields, &device.player_state.restrictions);
        device.player_state.restrictions = player_proto_Restrictions_init_zero;
        device.player_state.has_restrictions = false;
        this->putDeviceState(player_proto_connect_PutStateReason::player_proto_connect_PutStateReason_BECAME_INACTIVE);
        SC32_LOG(debug, "Device changed");
        sinkCommand(CommandType::DISC);
        //#ifndef CONFIG_SPOTIFY_STAY_CONNECTED_ON_TRANSFER
        this->disconnect();
        //#endif
      }
    }
    };

  this->ctx->session->addSubscriptionListener("social-connect",
    socialConnectSubscription);
  SC32_LOG(info, "Added social-connect supscription");

  ctx->session->setConnectedHandler([this]() {
    SC32_LOG(info, "Registered new device");
    this->putDeviceState(
      player_proto_connect_PutStateReason_SPIRC_HELLO);  // : player_proto_connect_PutStateReason::player_proto_connect_PutStateReason_NEW_DEVICE);
    // Assign country code
    this->ctx->config.countryCode = this->ctx->session->getCountryCode();
    });
  SC32_LOG(info, "Connecting to Spotify");
  this->ctx->session->connectWithRandomAp();
  SC32_LOG(info, "Connected to Spotify");
  ctx->config.authData = ctx->session->authenticate(blob);
  SC32_LOG(info, "Authenticated to Spotify blob size : %d",
    (int)ctx->config.authData.size());
  this->onTransfer = onTransfer;
  if (ctx->config.authData.size() > 0) {
    SC32_LOG(info, "Starting DeviceStateHandler");
    onLogin(blob->getUserName(), ctx->config.authData);
    SC32_LOG(info, "Called onLogin callback");
    this->onClose = onClose;
    this->trackQueue = std::make_shared<spotify::TrackQueue>(ctx);
    this->playerContext = std::make_shared<spotify::PlayerContext>(
      ctx, &this->device.player_state, &currentTracks, &offset);

    this->trackPlayer = std::make_shared<TrackPlayer>(
      ctx, trackQueue, [this](std::shared_ptr<spotify::QueuedTrack> track, spotify::TrackPlayer::State state) {
        this->setPlayerState(track, state);
      },
      &device.player_state.options.repeating_track);
    SC32_LOG(info, "Started player");

    device = {};
    // Prepare default device state
    device.has_device_info = true;
    // Prepare device info
    device.device_info.can_play = true;
    device.device_info.has_can_play = true;

    device.device_info.has_volume = true;
    device.device_info.volume = ctx->config.volume();

    device.device_info.name = strdup(ctx->config.deviceName.c_str());

    device.device_info.has_capabilities = true;
    device.device_info.capabilities = player_proto_connect_Capabilities{
        true,
        1,  //can_be_player
        true,
        1,  //restrict_to_local
        true,
        1,  //gaia_eq_connect_id
        true,
        1,  //supports_logout
        true,
        1,  //is_observable
        true,
        64,  //volume_steps
        0,
        NULL,  //{"audio/track", "audio/episode", "audio/episode+track"}, //supported_types
        true,
        1,  //command_acks
        false,
        0,  //supports_rename
        false,
        0,  //hidden
        true,
        0,  //disable_volume
        true,
        0,  //connect_disabled
        true,
        1,  //supports_playlist_v2
        true,
        1,  //is_controllable
        true,
        1,  //supports_external_episodes
        true,
        0,  //supports_set_backend_metadata
        true,
        1,  //supports_transfer_command
        true,
        0,  //supports_command_request
        false,
        0,  //is_voice_enabled
        true,
        0,  //needs_full_player_state //overuses MercuryManager, but keeps connection for outside wlan alive
        false,
        0,  //supports_gzip_pushes
        false,
        0,  //supports_lossless_audio
        true,
        1,  //supports_set_options_command
        true,  {true, 1, true, 1, true, 1} };
    device.device_info.capabilities.supported_types =
      (char**)calloc(5, sizeof(char*));
    device.device_info.capabilities.supported_types[0] = strdup("audio/track");
    device.device_info.capabilities.supported_types[1] = strdup("audio/episode");
    device.device_info.capabilities.supported_types[2] =
      strdup("audio/episode+track");
    device.device_info.capabilities.supported_types[3] =
      strdup("audio/interruption");
    device.device_info.capabilities.supported_types[4] = strdup("audio/local");
    device.device_info.capabilities.supported_types_count = 5;
    device.device_info.device_software_version = strdup(swVersion);
    device.device_info.has_device_type = true;
    device.device_info.device_type = player_proto_connect_DeviceType::player_proto_connect_DeviceType_SPEAKER;
    device.device_info.spirc_version = strdup(protocolVersion);
    device.device_info.device_id = strdup(ctx->config.deviceId.c_str());
    //device.device_info.client_id
    device.device_info.brand = strdup(brandName);
    device.device_info.model = strdup(informationString);
    //device.device_info.metadata_map = {{"debug_level","1"},{"tier1_port","0"},{"device_address_mask",local_ip}};
    //device.device_info.public_ip = ; // gets added trough server
    //device.device_info.license = ;
  }
  else {
    SC32_LOG(error, "Authentication failed, closing connection");
    this->disconnect();
    throw std::runtime_error("Failed to construct object.");
  }
}

DeviceStateHandler::~DeviceStateHandler() {
  if (isRunning.load()) disconnect();
  TrackReference::clearProvidedTracklist(&currentTracks);
  device.player_state.track = player_proto_connect_ProvidedTrack_init_zero;
  pb_release(player_proto_connect_Device_fields, &device);
  std::scoped_lock lock(deviceStateHandlerMutex);
  SC32_LOG(info, "DeviceStateHandler destroyed");
}

void DeviceStateHandler::runTask() {
  std::scoped_lock lock(deviceStateHandlerMutex);
  isRunning.store(true);
  while (isRunning) {

    try {
      ctx->session->handlePacket();
    }
    catch (std::exception& e) {
      SC32_LOG(error, "Error while connecting%s", e.what());
      //disconnect();
    }
  }
}

void DeviceStateHandler::setPlayerState(std::shared_ptr<spotify::QueuedTrack> track, spotify::TrackPlayer::State state) {
  this->device.player_state.timestamp =
    this->ctx->timeProvider->getSyncedTimestamp();
  switch (state) {
  case spotify::TrackPlayer::State::PLAYING:
    if (track->state != QueuedTrack::State::PLAYING) {
      track->state = QueuedTrack::State::PLAYING;
      this->device.player_state.timestamp =
        this->trackQueue->preloadedTracks[0]
        ->trackMetrics->currentInterval->start;

      this->device.player_state.duration = track->trackInfo.duration;
      sinkCommand(CommandType::PLAYBACK, trackQueue->preloadedTracks[0]);
    }
    break;
  case spotify::TrackPlayer::State::SEEKING:
    putPlayerState();
    break;
  case spotify::TrackPlayer::State::FAILED:
    if (track->ref.removed != NULL || track->state == QueuedTrack::State::PLAYING) {
      putPlayerState();
    }
    [[fallthrough]];
  case spotify::TrackPlayer::State::STOPPED:
    if (needsToBeSkipped) {
      if (this->device.player_state.options.repeating_track && state == spotify::TrackPlayer::State::STOPPED) {
        this->trackQueue->preloadedTracks[0]->requestedPosition = 0;
        this->trackQueue->preloadedTracks[0]->state = QueuedTrack::State::READY;
      }
      else if (this->trackQueue->preloadedTracks.size()) {
        SC32_LOG(info, "Skipping track");
        skip(CommandType::SKIP_NEXT, true);
        SC32_LOG(info, "Skipped track");
      }
    }
    needsToBeSkipped = true;
    SC32_LOG(info, "Stopping playback");
    if ((uint32_t)currentTracks.size() / 2 <= offset &&
      !this->resolvingContext) {
      this->resolvingContext.store(true);
      SC32_LOG(info, "Resolving tracklist");
      playerContext->resolveTracklist(metadata_map, reloadTrackList);
    }
    if (!this->trackQueue->preloadedTracks.size()) {
      SC32_LOG(info, "No more tracks");
      sinkCommand(CommandType::DISC);
    }
    else SC32_LOG(info, "preloadedTracks size: %d", this->trackQueue->preloadedTracks.size());
    break;
  default:
    break;
  }
}
void DeviceStateHandler::reloadTrackList(void* data) {
  handler->ctx->playbackMetrics->uri2context(handler->playerContext->context_uri);
  if (data == NULL) {
    if (handler->reloadPreloadedTracks) {
      handler->needsToBeSkipped = true;
      while (!handler->trackQueue->playableSemaphore->twait(1)) {};
      handler->trackPlayer->start();
      handler->trackPlayer->resetState();
      handler->reloadPreloadedTracks = false;
      handler->sinkCommand(CommandType::PLAYBACK_START);
      handler->device.player_state.track = handler->currentTracks[0];
    }
    if (!handler->offset) {
      if (handler->trackQueue->preloadedTracks.size())
        handler->trackQueue->preloadedTracks.clear();
      handler->trackQueue->preloadedTracks.push_back(
        std::make_shared<spotify::QueuedTrack>(
          handler->currentTracks[handler->offset], handler->ctx,
          handler->trackQueue->playableSemaphore,
          handler->offsetFromStartInMillis));
      handler->device.player_state.track =
        handler->currentTracks[handler->offset];
      handler->offsetFromStartInMillis = 0;
      handler->offset++;
    }
    if (!handler->trackQueue->preloadedTracks.size()) {
      handler->trackQueue->preloadedTracks.push_back(
        std::make_shared<spotify::QueuedTrack>(
          handler->currentTracks[handler->offset - 1], handler->ctx,
          handler->trackQueue->playableSemaphore,
          handler->offsetFromStartInMillis));
      handler->offsetFromStartInMillis = 0;
    }
    if (handler->currentTracks.size() >
      handler->trackQueue->preloadedTracks.size() + handler->offset) {
      while (handler->currentTracks.size() >
        handler->trackQueue->preloadedTracks.size() +
        handler->offset &&
        handler->trackQueue->preloadedTracks.size() < 3) {
        handler->trackQueue->preloadedTracks.push_back(
          std::make_shared<spotify::QueuedTrack>(
            handler->currentTracks
            [handler->offset +
            handler->trackQueue->preloadedTracks.size() - 1],
            handler->ctx, handler->trackQueue->playableSemaphore));
      }
    }
    if (handler->playerStateChanged) {
      handler->putPlayerState(
        player_proto_connect_PutStateReason::player_proto_connect_PutStateReason_PLAYER_STATE_CHANGED);
      handler->playerStateChanged = false;
    }
  }
  if (strcmp(handler->currentTracks[handler->offset - 1].uri,
    "spotify:delimiter") == 0 &&
    handler->device.player_state.is_playing &&
    handler->currentTracks.size() <= handler->offset) {
    handler->ctx->playbackMetrics->end_reason = spotify::PlaybackMetrics::REMOTE;
    handler->ctx->playbackMetrics->end_source = "unknown";
    handler->trackPlayer->stop();
    handler->device.player_state.has_is_playing = true;
    handler->device.player_state.is_playing = false;
    handler->device.player_state.track = player_proto_connect_ProvidedTrack_init_zero;
    handler->device.player_state.has_track = false;
    if (handler->device.player_state.has_restrictions)
      pb_release(player_proto_Restrictions_fields,
        &handler->device.player_state.restrictions);
    handler->device.player_state.restrictions = player_proto_Restrictions_init_zero;
    handler->device.player_state.has_restrictions = false;
    handler->putPlayerState();
    handler->sinkCommand(CommandType::DISC);
    //#ifndef CONFIG_SPOTIFY_STAY_CONNECTED_ON_TRANSFER
    handler->disconnect();
    //#endif
    return;
  }
  handler->resolvingContext.store(false);
  //SC32_LOG(info,"heap_memory_check-safe = %i",heap_caps_check_integrity_all(true));
}

void DeviceStateHandler::putDeviceState(player_proto_connect_PutStateReason put_state_reason) {
  //std::scoped_lock lock(playerStateMutex);
  std::string uri =
    "hm://connect-state/v1/devices/" + this->ctx->config.deviceId + "/";

  std::vector<player_proto_connect_ProvidedTrack> send_tracks = {};
  player_proto_connect_PutStateRequest tempPutReq = player_proto_connect_PutStateRequest_init_zero;
  tempPutReq.has_device = true;
  tempPutReq.has_member_type = true;
  tempPutReq.member_type = player_proto_connect_MemberType::player_proto_connect_MemberType_CONNECT_STATE;
  tempPutReq.has_is_active = true;
  tempPutReq.is_active = is_active;
  tempPutReq.has_put_state_reason = true;
  tempPutReq.put_state_reason = put_state_reason;
  tempPutReq.has_message_id = true;
  tempPutReq.message_id = last_message_id;
  tempPutReq.has_has_been_playing_for_ms = true;
  tempPutReq.has_been_playing_for_ms = (uint64_t)-1;
  tempPutReq.has_client_side_timestamp = true;
  tempPutReq.client_side_timestamp =
    this->ctx->timeProvider->getSyncedTimestamp();
  tempPutReq.has_only_write_player_state = true;
  tempPutReq.only_write_player_state = false;

  if (is_active) {
    tempPutReq.has_started_playing_at = true;
    tempPutReq.started_playing_at = this->started_playing_at;
    tempPutReq.has_been_playing_for_ms =
      this->ctx->timeProvider->getSyncedTimestamp() -
      this->started_playing_at;
    device.has_player_state = true;
    device.player_state.has_position_as_of_timestamp = true;
    device.player_state.position_as_of_timestamp =
      this->ctx->timeProvider->getSyncedTimestamp() -
      device.player_state.timestamp;
  }
  else
    device.has_player_state = false;
  device.player_state.next_tracks.funcs.encode =
    &spotify::TrackReference::pbEncodeProvidedTracks;
  device.player_state.next_tracks.arg = &queuePacket;
  device.device_info.volume = this->ctx->config.volume();
  tempPutReq.device = this->device;

  auto player_proto_connect_PutStateRequest = pbEncode(player_proto_connect_PutStateRequest_fields, &tempPutReq);
  tempPutReq.device = player_proto_connect_Device_init_zero;
  pb_release(player_proto_connect_PutStateRequest_fields, &tempPutReq);
  auto parts = MercurySession::DataParts({ player_proto_connect_PutStateRequest });
  auto responseLambda = [this](MercurySession::Response res) {
    if (res.fail || !res.parts.size())
      return;
    };
  this->ctx->session->execute(MercurySession::RequestType::PUT, uri,
    responseLambda, parts);
}

void DeviceStateHandler::putPlayerState(player_proto_connect_PutStateReason put_state_reason) {
  //std::scoped_lock lock(playerStateMutex);
  uint64_t now = this->ctx->timeProvider->getSyncedTimestamp();
  std::string uri =
    "hm://connect-state/v1/devices/" + this->ctx->config.deviceId + "/";
  player_proto_connect_PutStateRequest tempPutReq = {};
  pb_release(player_proto_connect_PutStateRequest_fields, &tempPutReq);
  tempPutReq = player_proto_connect_PutStateRequest_init_zero;
  tempPutReq.has_device = true;
  tempPutReq.has_member_type = false;
  tempPutReq.member_type = player_proto_connect_MemberType::player_proto_connect_MemberType_CONNECT_STATE;
  tempPutReq.has_is_active = true;
  tempPutReq.is_active = true;
  tempPutReq.has_put_state_reason = true;
  tempPutReq.put_state_reason = put_state_reason;
  tempPutReq.last_command_message_id = last_message_id;
  tempPutReq.has_started_playing_at = true;
  tempPutReq.started_playing_at = started_playing_at;
  tempPutReq.has_has_been_playing_for_ms = true;
  tempPutReq.has_been_playing_for_ms = now - started_playing_at;
  tempPutReq.has_client_side_timestamp = true;
  tempPutReq.client_side_timestamp = now;
  tempPutReq.has_only_write_player_state = true;
  tempPutReq.only_write_player_state = true;
  device.player_state.has_position_as_of_timestamp = true;
  device.player_state.has_timestamp = true;
  device.player_state.timestamp = now;
  device.device_info.volume = this->ctx->config.volume();
  device.has_player_state = true;
  device.player_state.position_as_of_timestamp = trackQueue->preloadedTracks[0]->trackMetrics->getPosition();
  queuePacket = { &offset, &currentTracks };
  device.player_state.next_tracks.funcs.encode =
    &spotify::TrackReference::pbEncodeProvidedTracks;
  device.player_state.next_tracks.arg = &queuePacket;
  if (device.player_state.track.provider &&
    strcmp(device.player_state.track.provider, "autoplay") == 0) {
    if (device.player_state.has_restrictions)
      pb_release(player_proto_Restrictions_fields, &device.player_state.restrictions);
    pb_release(player_proto_connect_ContextIndex_fields, &device.player_state.index);
    device.player_state.index = player_proto_connect_ContextIndex_init_zero;
    device.player_state.has_index = false;
    device.player_state.restrictions = player_proto_Restrictions_init_zero;
    if (!device.player_state.is_paused) {
      device.player_state.restrictions.disallow_resuming_reasons =
        (char**)calloc(1, sizeof(char*));
      device.player_state.restrictions.disallow_resuming_reasons_count = 1;
      device.player_state.restrictions.disallow_resuming_reasons[0] =
        strdup("not_paused");
    }
    else {
      device.player_state.restrictions.disallow_pausing_reasons =
        (char**)calloc(1, sizeof(char*));
      device.player_state.restrictions.disallow_pausing_reasons_count = 1;
      device.player_state.restrictions.disallow_pausing_reasons[0] =
        strdup("not_playing");
    }
    // Update player state with current track information
    device.player_state.restrictions.disallow_toggling_repeat_context_reasons =
      (char**)calloc(3, sizeof(char*));
    device.player_state.restrictions
      .disallow_toggling_repeat_context_reasons_count = 3;
    device.player_state.restrictions
      .disallow_toggling_repeat_context_reasons[0] = strdup("autoplay");
    device.player_state.restrictions
      .disallow_toggling_repeat_context_reasons[1] =
      strdup("endless_context");
    device.player_state.restrictions
      .disallow_toggling_repeat_context_reasons[2] = strdup("radio");

    device.player_state.restrictions.disallow_toggling_repeat_track_reasons =
      (char**)calloc(1, sizeof(char*));
    device.player_state.restrictions
      .disallow_toggling_repeat_track_reasons_count = 1;
    device.player_state.restrictions.disallow_toggling_repeat_track_reasons[0] =
      strdup("autoplay");

    device.player_state.restrictions.disallow_toggling_shuffle_reasons =
      (char**)calloc(3, sizeof(char*));
    device.player_state.restrictions.disallow_toggling_shuffle_reasons_count =
      3;
    device.player_state.restrictions.disallow_toggling_shuffle_reasons[0] =
      strdup("autoplay");
    device.player_state.restrictions.disallow_toggling_shuffle_reasons[1] =
      strdup("endless_context");
    device.player_state.restrictions.disallow_toggling_shuffle_reasons[2] =
      strdup("radio");

    device.player_state.restrictions.disallow_loading_context_reasons =
      (char**)calloc(1, sizeof(char*));
    device.player_state.restrictions.disallow_loading_context_reasons_count = 1;
    device.player_state.restrictions.disallow_loading_context_reasons[0] =
      strdup("not_supported_by_content_type");

    device.player_state.has_index = false;
    device.player_state.has_restrictions = true;
    if (device.player_state.play_origin.feature_classes != NULL) {
      free(device.player_state.play_origin.feature_classes);
      device.player_state.play_origin.feature_classes = NULL;
    }
  }
  else {
    device.player_state.index =
      player_proto_connect_ContextIndex{ true, device.player_state.track.page, true,
                   device.player_state.track.original_index };
    if (device.player_state.has_restrictions)
      pb_release(player_proto_Restrictions_fields, &device.player_state.restrictions);
    device.player_state.restrictions = player_proto_Restrictions_init_zero;
    if (!device.player_state.is_paused) {
      device.player_state.restrictions.disallow_resuming_reasons =
        (char**)calloc(1, sizeof(char*));
      device.player_state.restrictions.disallow_resuming_reasons_count = 1;
      device.player_state.restrictions.disallow_resuming_reasons[0] =
        strdup("not_paused");
    }
    else {
      device.player_state.restrictions.disallow_pausing_reasons =
        (char**)calloc(1, sizeof(char*));
      device.player_state.restrictions.disallow_pausing_reasons_count = 1;
      device.player_state.restrictions.disallow_pausing_reasons[0] =
        strdup("not_playing");
    }
    device.player_state.restrictions.disallow_loading_context_reasons =
      (char**)calloc(1, sizeof(char*));
    device.player_state.restrictions.disallow_loading_context_reasons_count = 1;
    device.player_state.restrictions.disallow_loading_context_reasons[0] =
      strdup("not_supported_by_content_type");

    device.player_state.has_restrictions = true;
  }
  tempPutReq.device = this->device;
  auto player_proto_connect_PutStateRequest = pbEncode(player_proto_connect_PutStateRequest_fields, &tempPutReq);
  tempPutReq.device = player_proto_connect_Device_init_zero;
  pb_release(player_proto_connect_PutStateRequest_fields, &tempPutReq);
  auto parts = MercurySession::DataParts({ player_proto_connect_PutStateRequest });

  auto responseLambda = [this](MercurySession::Response res) {
    if (res.fail || !res.parts.size())
      return;
    };
  this->ctx->session->execute(MercurySession::RequestType::PUT, uri,
    responseLambda, parts);
}

void DeviceStateHandler::disconnect(bool logout) {
  if (this->is_active) {
    this->is_active = false;
    this->ctx->playbackMetrics->end_reason = PlaybackMetrics::REMOTE;
    this->ctx->playbackMetrics->end_source = "unknown";
    if (device.player_state.has_restrictions)
      pb_release(player_proto_Restrictions_fields, &device.player_state.restrictions);
    device.player_state.restrictions = player_proto_Restrictions_init_zero;
    device.player_state.has_restrictions = false;
    this->putDeviceState(player_proto_connect_PutStateReason::player_proto_connect_PutStateReason_BECAME_INACTIVE);
    SC32_LOG(debug, "Device changed");
    sinkCommand(CommandType::DISC);
  }
  if (this->trackPlayer) this->trackPlayer->stop();
  if (this->trackQueue) {
    this->trackQueue->preloadedTracks.clear();
    this->trackQueue->stopTask();
  }
  this->ctx->session->disconnect();
  SC32_LOG(debug, "Disconnected from session");
  if (isRunning)
    onClose(logout);
  SC32_LOG(debug, "DeviceStateHandler disconnected");
  this->isRunning.store(false);
}

void DeviceStateHandler::skip(CommandType dir, bool notify) {
  if (dir == CommandType::SKIP_NEXT) {
    this->device.player_state.track = currentTracks[offset];
    if (this->device.player_state.track.full_metadata_count >
      this->device.player_state.track.metadata_count)
      this->device.player_state.track.metadata_count =
      this->device.player_state.track.full_metadata_count;
    if (trackQueue->preloadedTracks.size()) {
      trackQueue->preloadedTracks.pop_front();
      if (currentTracks.size() >
        (trackQueue->preloadedTracks.size() + offset)) {
        while (currentTracks.size() >
          trackQueue->preloadedTracks.size() + offset &&
          trackQueue->preloadedTracks.size() < 3) {
          trackQueue->preloadedTracks.push_back(
            std::make_shared<spotify::QueuedTrack>(
              currentTracks[offset + trackQueue->preloadedTracks.size()],
              this->ctx, this->trackQueue->playableSemaphore));
        }
      }
      offset++;
    }
  }
  else if (trackQueue->preloadedTracks[0]->trackMetrics->getPosition() >=
    3000 &&
    offset > 1) {
    trackQueue->preloadedTracks.pop_back();
    offset--;
    trackQueue->preloadedTracks.push_front(std::make_shared<spotify::QueuedTrack>(
      currentTracks[offset - 1], this->ctx,
      this->trackQueue->playableSemaphore));
  }
  else {
    if (trackQueue->preloadedTracks.size())
      trackQueue->preloadedTracks[0]->requestedPosition = 0;
  }
  if (trackQueue->preloadedTracks.size() &&
    currentTracks.size() < offset + trackQueue->preloadedTracks.size()) {
    playerContext->resolveTracklist(metadata_map, reloadTrackList);
  }
  if (!trackQueue->preloadedTracks.size()) {
    trackPlayer->resetState();
    reloadPreloadedTracks = true; // In case there are incoming tracks
  }
  else if (!notify)
    trackPlayer->resetState();
}

void DeviceStateHandler::parseCommand(std::vector<uint8_t>& data) {
  if (data.size() <= 2)
    return;
  nlohmann::json jsonResult;
  try {
    jsonResult = nlohmann::json::parse(data);
  }
  catch (const nlohmann::json::parse_error&) {
    SC32_LOG(error, "Failed to parse command");
    return;  // Parsing failed
  }

  last_message_id = jsonResult.value("message_id", last_message_id);

  auto command = jsonResult.find("command");

  if (command != jsonResult.end()) {
    if (command->find("endpoint") == command->end())
      return;
    SC32_LOG(debug, "Parsing new command, endpoint : %s",
      command->at("endpoint").get<std::string>().c_str());
    auto options = command->find("options");
    if (command->at("endpoint") == "transfer") {
      if (is_active)
        return;
      if (options != command->end()) {
        if (options->find("restore_paused") !=
          options->end()) {  //"restore"==play
          if (!is_active && options->at("restore_paused") == "restore") {
            started_playing_at = this->ctx->timeProvider->getSyncedTimestamp();
            is_active = true;
          }
        }
      }
      if (this->playerContext->next_page_url != NULL)
        unreference(&(this->playerContext->next_page_url));
      this->playerContext->radio_offset = 0;
      this->device.player_state.has_timestamp = true;
      this->device.player_state.timestamp =
        this->ctx->timeProvider->getSyncedTimestamp();
      if (!is_active) {
        this->trackPlayer->start();
        started_playing_at = this->device.player_state.timestamp;
        is_active = true;
      }
      auto logging_params = command->find("logging_params");

      player_proto_transfer_TransferState transfer_state = {};
      const std::string& s = command->at("data").get_ref<const std::string&>();
      std::vector<uint8_t> bytes = base64ToBytes(s);  // UTF-8 bytes of the string

      pbDecode(transfer_state, player_proto_transfer_TransferState_fields, bytes);

      spotify::TrackReference::clearProvidedTracklist(&currentTracks);
      currentTracks = {};

      if (transfer_state.has_options) PB_MOVE_ASSIGN(player_proto_ContextPlayerOptions, device.player_state.options, transfer_state.options);
      if (transfer_state.current_session.has_option_overrides) {
        if (transfer_state.current_session.option_overrides.has_repeating_context) {
          this->device.player_state.options.repeating_context = transfer_state.current_session.option_overrides.repeating_context;
          this->device.player_state.options.has_repeating_context = true;
        }
        if (transfer_state.current_session.option_overrides.has_repeating_track) {
          this->device.player_state.options.repeating_track = transfer_state.current_session.option_overrides.repeating_track;
          this->device.player_state.options.has_repeating_track = true;
        }
        if (transfer_state.current_session.option_overrides.has_shuffling_context) {
          this->device.player_state.options.shuffling_context = transfer_state.current_session.option_overrides.shuffling_context;
          this->device.player_state.options.has_shuffling_context = true;
        }
      }
      if (pb_map_get_value<MetadataEntry>(
        transfer_state.current_session.context.metadata,
        transfer_state.current_session.context.metadata_count,
        "enhanced_context")
        != NULL) {
        SC32_LOG(debug, "Enhanced context");
        if (this->device.player_state.options.context_enhancement_count) {
          for (int i = 0; i < this->device.player_state.options.context_enhancement_count; i++) {
            pb_release(MetadataEntry_fields, &this->device.player_state.options.context_enhancement[i]);
          }
          free(this->device.player_state.options.context_enhancement);
          this->device.player_state.options.context_enhancement = NULL;
          this->device.player_state.options.context_enhancement_count = 0;
        }
        this->device.player_state.options.context_enhancement = (MetadataEntry*)calloc(1, sizeof(MetadataEntry));
        this->device.player_state.options.context_enhancement[0].key =
          strdup("context_enhancement");
        this->device.player_state.options.context_enhancement[0].value =
          strdup("NONE");
        this->device.player_state.options.context_enhancement_count = 1;
      }
      device.player_state.has_options = true;
      this->device.player_state.context_metadata = transfer_state.current_session.context.metadata;
      this->device.player_state.context_metadata_count = transfer_state.current_session.context.metadata_count;
      transfer_state.current_session.context.metadata = NULL;
      transfer_state.current_session.context.metadata_count = 0;
      offsetFromStartInMillis = transfer_state.playback.position_as_of_timestamp;
      if (transfer_state.playback.has_timestamp) offsetFromStartInMillis += (int64_t)this->ctx->timeProvider->getSyncedTimestamp() - transfer_state.playback.timestamp;
      this->device.player_state.has_is_playing = true;
      this->device.player_state.is_playing = true;
      char* temp = pb_map_get_value<MetadataEntry>(transfer_state.playback.current_track.metadata, transfer_state.playback.current_track.metadata_count, "interaction_id");
      if (temp != NULL) {
        metadata_map.push_back(std::make_pair("interaction_id", std::string(temp)));
      }
      else if (logging_params != command->end() && logging_params->find("interaction_ids") != logging_params->end()) {
        metadata_map.push_back(std::make_pair("interaction_id", logging_params->at("interaction_ids")[0].get<std::string>()));
      }
      temp = pb_map_get_value<MetadataEntry>(transfer_state.playback.current_track.metadata, transfer_state.playback.current_track.metadata_count, "page_instance_id");
      if (temp != NULL) {
        metadata_map.push_back(std::make_pair("page_instance_id", std::string(temp)));
      }
      else if (logging_params != command->end() && logging_params->find("page_instance_ids") != logging_params->end()) {
        metadata_map.push_back(std::make_pair("page_instance_id", logging_params->at("page_instance_ids")[0].get<std::string>()));
      }
      player_proto_connect_ProvidedTrack track = player_proto_connect_ProvidedTrack_init_default;
      move_ContextTrack_to_ProvidedTrack(&transfer_state.playback.current_track, &track);
      currentTracks.push_back(track);
      char* compare = NULL;
      if (track.uid != NULL && track.uid[0] != '\0') {
        compare = track.uid;
      }
      else if (track.uri != NULL && track.uri[0] != '\0') {
        compare = track.uri;
      }
      track = player_proto_connect_ProvidedTrack_init_default;
      player_proto_ContextPage* pages = transfer_state.current_session.context.pages;
      if (pages != NULL) {
        for (int i = 0; i < pages->tracks_count; i++) {
          if (strcmp(compare, pages->tracks[i].uri) == 0 || strcmp(compare, pages->tracks[i].uid) == 0) {
            continue;
          }
          move_ContextTrack_to_ProvidedTrack(&pages->tracks[i], &track);
          currentTracks.push_back(track);
          track = player_proto_connect_ProvidedTrack_init_default;
        }
      }

      this->device.player_state.track = currentTracks[0];
      this->device.player_state.has_track = true;
      unreference(&(this->device.player_state.context_uri));
      unreference(&(this->device.player_state.context_url));
      this->device.player_state.context_uri = transfer_state.current_session.context.uri;
      this->device.player_state.context_url = transfer_state.current_session.context.url;
      transfer_state.current_session.context.uri = NULL;
      transfer_state.current_session.context.url = NULL;
      std::vector<uint8_t> random_bytes;
      static std::uniform_int_distribution<int> d(0, 255);
      for (int i = 0; i < 16; i++) {
        random_bytes.push_back(d(ctx->rng));
      }
      unreference(&(this->device.player_state.session_id));
      this->device.player_state.session_id =
        strdup(bytesToHexString(random_bytes).c_str());

      unreference(&(this->device.player_state.playback_id));
      random_bytes.clear();
      for (int i = 0; i < 16; i++) {
        random_bytes.push_back(d(ctx->rng));
      }
      this->device.player_state.playback_id =
        strdup(base64Encode(random_bytes).c_str());

      offset = 0;

      this->device.player_state.is_playing = true;
      this->device.player_state.has_track = true;
      this->device.player_state.has_is_paused = true;
      this->device.player_state.is_paused = false;
      this->device.player_state.has_is_playing = true;
      this->device.player_state.has_position_as_of_timestamp = true;
      this->device.player_state.position_as_of_timestamp = offsetFromStartInMillis;
      this->device.player_state.has_timestamp = true;
      this->device.player_state.timestamp = ctx->timeProvider->getSyncedTimestamp();
      this->device.player_state.playback_speed = 1.0;
      this->device.player_state.has_playback_speed = true;

      this->device.player_state.has_duration = true;
      this->device.player_state.duration = 0;
      this->device.player_state.has_position = true;
      this->device.player_state.position = 0;

      queuePacket = { &offset, &currentTracks };
      onTransfer();
      this->putDeviceState(
        player_proto_connect_PutStateReason::player_proto_connect_PutStateReason_PLAYER_STATE_CHANGED);
      this->device.has_player_state = true;
      reloadPreloadedTracks = true;
      playerContext->resolveTracklist(metadata_map, reloadTrackList, true, true);
      pb_release(player_proto_transfer_TransferState_fields, &transfer_state);
    }
    else if (this->is_active) {
      if (command->at("endpoint") == "play") {
#ifndef CONFIG_BELL_NOCODEC
        handler->trackPlayer->stop();
        sinkCommand(CommandType::DEPLETED);
#endif
        // reset the player context
        if (this->playerContext->next_page_url != NULL)
          unreference(&(this->playerContext->next_page_url));
        this->playerContext->radio_offset = 0;
        trackQueue->preloadedTracks.clear();
        uint8_t queued = 0;
        if (!this->device.player_state.is_playing) {
          this->device.player_state.is_playing = true;
          this->device.player_state.has_track = true;
        }
        // Remove non-queue tracks
        remove_tracks_by_provider(currentTracks, "queue", 0, true);
        auto logging_params = command->find("logging_params");
        if (logging_params != command->end()) {
          metadata_map.clear();
          if (logging_params->find("page_instance_ids") !=
            logging_params->end()) {
            metadata_map.push_back(std::make_pair(
              "page_instance_ids",
              logging_params->at("page_instance_ids")[0].get<std::string>()));
          }
          if (logging_params->find("interaction_ids") !=
            logging_params->end()) {
            metadata_map.push_back(std::make_pair(
              "interaction_id",
              logging_params->at("interaction_ids")[0].get<std::string>()));
          }
        }
        if (command->find("play_origin") != command->end()) {
          pb_release(player_proto_connect_PlayOrigin_fields, &device.player_state.play_origin);
          device.player_state.play_origin = player_proto_connect_PlayOrigin_init_zero;
          device.player_state.play_origin.feature_identifier =
            PlayerContext::createStringReferenceIfFound(
              command->at("play_origin"), "feature_identifier");
          device.player_state.play_origin.feature_version =
            PlayerContext::createStringReferenceIfFound(
              command->at("play_origin"), "feature_version");
          device.player_state.play_origin.referrer_identifier =
            PlayerContext::createStringReferenceIfFound(
              command->at("play_origin"), "referrer_identifier");
        }

        auto options = command->find("options");
        int64_t playlist_offset = 0;
        if (options != command->end()) {
          if (options->find("player_options_override") != options->end() &&
            options->at("player_options_override")
            .find("shuffling_context") !=
            options->at("player_options_override").end())
            device.player_state.options.shuffling_context =
            options->at("player_options_override").at("shuffling_context");
          else
            device.player_state.options.shuffling_context = false;
          if (options->find("skip_to") != options->end()) {
            if (options->at("skip_to").size()) {
              if (options->at("skip_to").find("track_index") !=
                options->at("skip_to").end()) {
                playlist_offset = options->at("skip_to").at("track_index");
              }
            }
          }
        }

        // CONTEXT 
                // reset the context_uri and context_url
        unreference(&(this->device.player_state.context_uri));
        this->device.player_state.context_uri =
          PlayerContext::createStringReferenceIfFound(command->at("context"),
            "uri");
        unreference(&(this->device.player_state.context_url));
        this->device.player_state.context_url =
          PlayerContext::createStringReferenceIfFound(command->at("context"),
            "url");
        // reset the context metadata
        auto metadata = command->at("context").find("metadata");
        if (metadata != command->at("context").end()) {
          if (this->device.player_state.options
            .context_enhancement_count) {
            for (int i = 0; i < this->device.player_state.options.context_enhancement_count; i++) {
              pb_release(MetadataEntry_fields, &this->device.player_state.options.context_enhancement[i]);
            }
            free(this->device.player_state.options.context_enhancement);
            this->device.player_state.options.context_enhancement = NULL;
            this->device.player_state.options.context_enhancement_count = 0;
          }
          if (metadata->find("enhanced_context") != metadata->end() && !metadata->at("enhanced_context").is_boolean()) {
            this->device.player_state.options.context_enhancement = (MetadataEntry*)calloc(1, sizeof(MetadataEntry));
            this->device.player_state.options.context_enhancement[0].key =
              strdup("context_enhancement");
            this->device.player_state.options.context_enhancement[0].value =
              strdup("NONE");
            this->device.player_state.options.context_enhancement_count = 1;
          }

          context_metadata_map.clear();
          if (metadata->find("context_description") != metadata->end()) {
            context_metadata_map.push_back(std::make_pair(
              "context_description",
              metadata->at("context_description").get<std::string>()));
          }
          if (metadata->find("context_owner") != metadata->end()) {
            context_metadata_map.push_back(std::make_pair(
              "context_owner",
              metadata->at("context_owner").get<std::string>()));
          }
          for (int i = 0; i < this->device.player_state.context_metadata_count; i++) {
            pb_release(MetadataEntry_fields, &device.player_state.context_metadata[i]);
          }
          free(this->device.player_state.context_metadata);
          this->device.player_state.context_metadata =
            (MetadataEntry*)calloc(
              context_metadata_map.size(),
              sizeof(MetadataEntry));
          for (int i = 0; i < context_metadata_map.size(); i++) {
            this->device.player_state.context_metadata[i].key =
              strdup(context_metadata_map[i].first.c_str());
            this->device.player_state.context_metadata[i].value =
              strdup(context_metadata_map[i].second.c_str());
          }
          this->device.player_state.context_metadata_count =
            context_metadata_map.size();
        }
        else {
          if (this->device.player_state.options.context_enhancement_count) {
            for (int i = 0; i < this->device.player_state.options.context_enhancement_count; ++i) {
              pb_release(MetadataEntry_fields, &device.player_state.options.context_enhancement[i]);
            }
            free(this->device.player_state.options.context_enhancement);
            this->device.player_state.options.context_enhancement = NULL;
            this->device.player_state.options.context_enhancement_count = 0;
          }
          context_metadata_map.clear();
          for (int i = 0; i < this->device.player_state.options.context_enhancement_count; ++i) {
            pb_release(MetadataEntry_fields, &device.player_state.context_metadata[i]);
          }
          free(this->device.player_state.context_metadata);
          this->device.player_state.context_metadata = NULL;
          this->device.player_state.context_metadata_count = 0;
        }
        reloadPreloadedTracks = true;
        std::string provider = "";
        if (auto* url = this->device.player_state.context_url) {
          if (const char* colon = strchr(url, ':')) provider.assign(url, colon - url);
        }
        else if (strchr(this->device.player_state.context_uri, ':') !=
          strrchr(this->device.player_state.context_uri, ':')) {
          provider = "context";
        }
        if (options->find("skip_to")->find("track_uri") != options->find("skip_to")->end()) {
          player_proto_connect_ProvidedTrack track = player_proto_connect_ProvidedTrack_init_zero;
          track.original_index = playlist_offset;
          track.uri = PlayerContext::createStringReferenceIfFound(
            options->at("skip_to"), "track_uri");
          track.uid = PlayerContext::createStringReferenceIfFound(
            options->at("skip_to"), "track_uid");
          track.provider = strdup(provider.c_str());
          currentTracks.push_back(track);
        }
        else if (command->at("context").find("pages") !=
          command->at("context").end() &&
          command->at("context").at("pages")[0]["tracks"].size() >
          playlist_offset) {
          auto json_track = command->at("context")["pages"][0]["tracks"].at(playlist_offset);
          player_proto_connect_ProvidedTrack track = player_proto_connect_ProvidedTrack_init_zero;
          track.original_index = playlist_offset;
          track.uri = PlayerContext::createStringReferenceIfFound(
            json_track, "uri");
          track.uid = PlayerContext::createStringReferenceIfFound(
            json_track, "uid");
          SC32_LOG(info, "track uri: %s", track.uri);
          track.provider = strdup(provider.c_str());
          currentTracks.push_back(track);
        }
        offset = 0;
        SC32_LOG(info, "Tracklist reloaded");
        this->playerContext->resolveTracklist(metadata_map, reloadTrackList,
          true, true);
        SC32_LOG(info, "Tracklist reloaded");
      }
      else if (command->at("endpoint") == "pause") {
        device.player_state.is_paused = true;
        device.player_state.has_is_paused = true;
        this->putPlayerState();
        sinkCommand(CommandType::PAUSE);
      }
      else if (command->at("endpoint") == "resume") {
        device.player_state.is_paused = false;
        device.player_state.has_is_paused = true;
        this->putPlayerState();
        sinkCommand(CommandType::PLAY);
      }
      else if (command->at("endpoint") == "skip_next") {
        ctx->playbackMetrics->end_reason = PlaybackMetrics::FORWARD_BTN;
#ifndef CONFIG_BELL_NOCODEC
        this->needsToBeSkipped = false;
#endif
        if (command->find("track") == command->end())
          skip(CommandType::SKIP_NEXT, false);
        else {
          offset = 0;
          for (auto track : currentTracks) {
            if (strcmp(command->find("track")
              ->at("uri")
              .get<std::string>()
              .c_str(),
              track.uri) == 0)
              break;
            offset++;
          }
          trackQueue->preloadedTracks.clear();

          this->device.player_state.track = currentTracks[offset];
          for (auto i = offset;
            i < (currentTracks.size() < 3 + offset ? currentTracks.size()
              : 3 + offset);
            i++) {
            trackQueue->preloadedTracks.push_back(
              std::make_shared<spotify::QueuedTrack>(
                currentTracks[i], this->ctx,
                this->trackQueue->playableSemaphore));
          }
          offset++;
          trackPlayer->resetState();
        }
        sinkCommand(CommandType::SKIP_NEXT);
      }
      else if (command->at("endpoint") == "skip_prev") {
        ctx->playbackMetrics->end_reason = PlaybackMetrics::BACKWARD_BTN;
        needsToBeSkipped = false;
        skip(CommandType::SKIP_PREV, false);
        sinkCommand(CommandType::SKIP_PREV);

      }
      else if (command->at("endpoint") == "seek_to") {

#ifndef CONFIG_BELL_NOCODEC
        if (!this->trackQueue->preloadedTracks[0]->loading)
          needsToBeSkipped = false;
#endif
        if (command->at("relative") == "beginning") {  //relative
          this->device.player_state.has_position_as_of_timestamp = true;
          this->device.player_state.position_as_of_timestamp =
            command->at("value").get<int64_t>();
          this->device.player_state.timestamp =
            this->ctx->timeProvider->getSyncedTimestamp();
          this->trackPlayer->seekMs(
            command->at("value").get<uint32_t>(),
            this->trackQueue->preloadedTracks[0]->loading);
        }
        else if (command->at("relative") == "current") {
          this->device.player_state.has_position_as_of_timestamp = true;
          this->device.player_state.position_as_of_timestamp =
            command->at("value").get<int64_t>() +
            command->at("position").get<int64_t>();
          this->trackPlayer->seekMs(
            this->device.player_state.position_as_of_timestamp,
            this->trackQueue->preloadedTracks[0]->loading);
          this->device.player_state.timestamp =
            this->ctx->timeProvider->getSyncedTimestamp();
        }
        sinkCommand(
          CommandType::SEEK,
          (int32_t)this->device.player_state.position_as_of_timestamp);
        this->putPlayerState();
      }
      else if (command->at("endpoint") == "add_to_queue") {
        uint8_t queuedOffset = 0;
        //look up already queued tracks
        for (uint8_t i = offset; i < currentTracks.size(); i++) {
          if (strcmp(currentTracks[i].provider, "queue") != 0)
            break;
          queuedOffset++;
        }

        player_proto_connect_ProvidedTrack track = {};
        track.uri = strdup(
          command->find("track")->at("uri").get<std::string>().c_str());
        track.provider = strdup("queue");
        this->currentTracks.insert(
          this->currentTracks.begin() + offset + queuedOffset, track);
        if (queuedOffset < 2) {
          trackQueue->preloadedTracks.pop_back();
          trackQueue->preloadedTracks.insert(
            trackQueue->preloadedTracks.begin() + 1 + queuedOffset,
            std::make_shared<spotify::QueuedTrack>(
              currentTracks[offset + queuedOffset], this->ctx,
              this->trackQueue->playableSemaphore));
        }
#ifndef CONFIG_BELL_NOCODEC
        this->trackPlayer->seekMs(
          trackQueue->preloadedTracks[0]->trackMetrics->getPosition(),
          this->trackQueue->preloadedTracks[0]->loading);
        sinkCommand(
          CommandType::SEEK,
          (int32_t)this->device.player_state.position_as_of_timestamp);
#endif
        this->putPlayerState();
      }
      else if (command->at("endpoint") == "set_queue") {
        SC32_LOG(info, "%s", jsonResult.dump().c_str());
        uint8_t queuedOffset = 0, newQueuedOffset = 0;
        //look up already queued tracks
        for (uint8_t i = offset; i < currentTracks.size(); i++) {
          if (strcmp(currentTracks[i].provider, "queue") != 0)
            break;
          queuedOffset++;
        }
        auto tracks = command->find("next_tracks");
        if (tracks != command->end()) {
          int trackoffset = offset;
          int jsonoffset = 0;
          while (jsonoffset < tracks->size() && trackoffset < currentTracks.size()) {
            if (!tracks->at(jsonoffset).contains("provider") || tracks->at(jsonoffset).at("provider") == "queue") {
              if (tracks->at(jsonoffset)["uri"] == tracks->at(trackoffset).at("uri")) {
                trackoffset++;
              }
              else {
                //Add track to back of queue                
                player_proto_connect_ProvidedTrack track = {};
                track.uri = strdup(tracks->at(jsonoffset).at("uri").get<std::string>().c_str());
                track.provider = strdup("queue");
                this->currentTracks.insert(
                  this->currentTracks.begin() + offset + queuedOffset + newQueuedOffset, track);
                newQueuedOffset++;
              }
              jsonoffset++;
            }
            else break;
          }
        }
        if (queuedOffset < 2 || newQueuedOffset < 2) {
          trackQueue->preloadedTracks.clear();
          while (trackQueue->preloadedTracks.size() < 3)
            trackQueue->preloadedTracks.push_back(
              std::make_shared<spotify::QueuedTrack>(
                currentTracks[offset + trackQueue->preloadedTracks.size() -
                1],
                this->ctx, this->trackQueue->playableSemaphore));
        }
#ifndef CONFIG_BELL_NOCODEC
        this->trackPlayer->seekMs(
          trackQueue->preloadedTracks[0]->trackMetrics->getPosition(),
          this->trackQueue->preloadedTracks[0]->loading);
        sinkCommand(
          CommandType::SEEK,
          (int32_t)this->device.player_state.position_as_of_timestamp);
#endif
        this->putPlayerState();
      }
      else if (command->at("endpoint") == "update_context") {
        unreference(&(this->device.player_state.session_id));
        this->device.player_state.session_id =
          PlayerContext::createStringReferenceIfFound(*command, "session_id");

        auto context = command->find("context");
        if (context != command->end()) {
          if (context_metadata_map.size())
            context_metadata_map.clear();
          context_uri = context->find("uri") != context->end()
            ? context->at("uri").get<std::string>()
            : " ";
          context_url = context->find("url") != context->end()
            ? context->at("url").get<std::string>()
            : " ";
          auto metadata = context->find("metadata");
          if (metadata != context->end()) {
            for (auto element : metadata->items()) {
              if (element.value().size() && element.value() != "") {
                context_metadata_map.push_back(std::make_pair(
                  element.key(), element.value().get<std::string>()));
              }
            }
          }
        }
      }
      else if (command->at("endpoint") == "set_shuffling_context") {
        if (context_uri.size()) {
          unreference(&(this->device.player_state.context_uri));
          this->device.player_state.context_uri = strdup(context_uri.c_str());
        }
        if (context_url.size()) {
          unreference(&(this->device.player_state.context_url));
          this->device.player_state.context_url = strdup(context_url.c_str());
        }
        for (int i = 0; i < this->device.player_state.context_metadata_count; ++i) {
          pb_release(MetadataEntry_fields, &device.player_state.context_metadata[i]);
        }
        free(this->device.player_state.context_metadata);
        this->device.player_state.context_metadata =
          (MetadataEntry*)calloc(
            context_metadata_map.size(),
            sizeof(MetadataEntry));
        for (int i = 0; i < context_metadata_map.size(); i++) {
          this->device.player_state.context_metadata[i].key =
            strdup(context_metadata_map[i].first.c_str());
          this->device.player_state.context_metadata[i].value =
            strdup(context_metadata_map[i].second.c_str());
        }
        this->device.player_state.context_metadata_count =
          context_metadata_map.size();

        this->device.player_state.has_options = true;
        this->device.player_state.options.has_shuffling_context = true;
        if (command->find("value").value())
          this->device.player_state.options.shuffling_context = true;
        else
          this->device.player_state.options.shuffling_context = false;
        for (int i = 0; i < this->device.player_state.options.context_enhancement_count; ++i) {
          pb_release(MetadataEntry_fields, &device.player_state.options.context_enhancement[i]);
        }
        free(this->device.player_state.options.context_enhancement);
        this->device.player_state.options.context_enhancement_count = 0;
        this->device.player_state.options.context_enhancement = NULL;
        if (strchr(this->device.player_state.context_url, '?') != NULL) {
          this->device.player_state.options.context_enhancement = (MetadataEntry*)calloc(1, sizeof(MetadataEntry));
          this->device.player_state.options.context_enhancement[0].key =
            strdup("context_enhancement");
          this->device.player_state.options.context_enhancement[0].value =
            strdup("NONE");
          this->device.player_state.options.context_enhancement_count = 1;
        }
        playerStateChanged = true;
        this->trackQueue->preloadedTracks.clear();
        remove_tracks_by_provider(currentTracks, "queue", offset, true);
        playerContext->resolveTracklist(metadata_map, reloadTrackList, true);
        sinkCommand(CommandType::SET_SHUFFLE,
          (int32_t)(this->device.player_state.options
            .context_enhancement_count
            ? 2
            : this->device.player_state.options
            .shuffling_context));
#ifndef CONFIG_BELL_NOCODEC
        this->trackPlayer->seekMs(
          trackQueue->preloadedTracks[0]->trackMetrics->getPosition(),
          this->trackQueue->preloadedTracks[0]->loading);
        sinkCommand(
          CommandType::SEEK,
          (int32_t)this->device.player_state.position_as_of_timestamp);
#endif
      }
      else if (command->at("endpoint") == "set_options") {

        if (this->device.player_state.options.repeating_context !=
          command->at("repeating_context").get<bool>()) {
          uint8_t release = 0;
          for (int i = offset; i < currentTracks.size(); i++)
            if (strcmp(currentTracks[i].uri, "spotify:delimiter") == 0 ||
              strcmp(currentTracks[i].provider, "autoplay") == 0) {
              release = i;
              break;
            }
          if (release) {
            for (int i = release; i < currentTracks.size(); i++)
              spotify::TrackReference::pbReleaseProvidedTrack(&currentTracks[i]);
            currentTracks.erase(currentTracks.begin() + release,
              currentTracks.end());
          }

          this->device.player_state.options.has_repeating_context = true;
          this->device.player_state.options.repeating_context =
            command->at("repeating_context").get<bool>();
          this->device.player_state.options.repeating_track =
            command->at("repeating_track").get<bool>();
          this->device.player_state.options.has_repeating_track = true;
          playerStateChanged = true;
          this->playerContext->resolveTracklist(metadata_map, reloadTrackList,
            true);
        }
        else {
          this->device.player_state.options.has_repeating_context = true;
          this->device.player_state.options.repeating_context =
            command->at("repeating_context").get<bool>();
          this->device.player_state.options.repeating_track =
            command->at("repeating_track").get<bool>();
          this->device.player_state.options.has_repeating_track = true;
          this->putPlayerState();
        }
        if (this->device.player_state.options.repeating_context)
          sinkCommand(
            CommandType::SET_REPEAT,
            (int32_t)(this->device.player_state.options.repeating_context
              ? 2
              : this->device.player_state.options
              .repeating_track));
      }
      else {
        SC32_LOG(error, "Unknown command: %s",
          &command->at("endpoint").get<std::string>()[0]);
        SC32_LOG(debug, "data: %s", command->dump(2).c_str());
      }
      return;
    }
  }
}
void DeviceStateHandler::sinkCommand(CommandType type, CommandData data) {
  Command command;
  command.commandType = type;
  command.data = data;
  stateToSinkCallback(command);
}