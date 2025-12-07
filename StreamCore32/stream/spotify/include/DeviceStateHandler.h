/**
 * TO DO
 * 
 * autoplay doesn't work for episodes
 * 
 */
#pragma once

#include <stdint.h>    // for uint8_t, uint32_t
#include <atomic>      // for atomic
#include <deque>       //for deque..
#include <functional>  //for function
#include <memory>      // for shared_ptr
#include <string>      // for string
#include <utility>     // for pair
#include <variant>     // for variant
#include <vector>      // for vector

#include "PlayerContext.h"  // for PlayerContext::resolveTracklist, jsonToTracklist...
#include "SpotifyCommand.h"  // for Command, CommandType, CommandData
#include "TrackPlayer.h"     // for TrackPlayer
#include "TrackQueue.h"
#include "TrackReference.h"
#include "protobuf/connect.pb.h"  // for PutStateRequest, DeviceState, PlayerState...

namespace spotify {
struct Context;
struct PlayerContext;

class DeviceStateHandler : public bell::Task {
 public:
  //Command Callback Structure

  typedef std::function<void(Command)> StateCallback;
  std::function<void(bool)> onClose = [](bool logout) {
  };

  DeviceStateHandler(
      std::shared_ptr<spotify::LoginBlob>, std::function<void(bool)>,
      std::function<void()>,
      std::function<void(const std::string&, const std::vector<uint8_t>&)>,
      std::function<uint16_t()>);
  ~DeviceStateHandler();

  void disconnect(bool logout = false);

  void putDeviceState(
      player_proto_connect_PutStateReason member_type =
          player_proto_connect_PutStateReason::
              player_proto_connect_PutStateReason_PLAYER_STATE_CHANGED);

  void setDeviceState(player_proto_connect_PutStateReason put_state_reason);
  void putPlayerState(
      player_proto_connect_PutStateReason member_type =
          player_proto_connect_PutStateReason::
              player_proto_connect_PutStateReason_PLAYER_STATE_CHANGED);
  void handleConnectState();

  void sinkCommand(CommandType, CommandData data = {});

  void setPlayerState(std::shared_ptr<spotify::QueuedTrack> track,
                      spotify::TrackPlayer::State state);

  void endTrack(bool loaded);

  player_proto_connect_Device device = player_proto_connect_Device_init_zero;

  std::vector<player_proto_connect_ProvidedTrack> currentTracks = {};
  StateCallback stateToSinkCallback;

  uint64_t started_playing_at = 0;
  uint32_t last_message_id = -1;
  uint8_t offset = 0;
  int64_t offsetFromStartInMillis = 0;

  bool is_active = false;
  bool reloadPreloadedTracks = true;
  bool needsToBeSkipped = true;
  bool playerStateChanged = false;
  std::atomic<bool> isRunning = false;

  std::shared_ptr<spotify::TrackPlayer> trackPlayer;
  std::shared_ptr<spotify::TrackQueue> trackQueue;
  std::shared_ptr<spotify::Context> ctx;

 private:
  std::shared_ptr<spotify::PlayerContext> playerContext;
  std::atomic<bool> logoutRequest_ = false;
  std::pair<uint8_t*, std::vector<player_proto_connect_ProvidedTrack>*>
      queuePacket = {&offset, &currentTracks};
  std::vector<std::pair<std::string, std::string>> metadata_map = {};
  std::vector<std::pair<std::string, std::string>> context_metadata_map = {};
  std::function<void()> onTransfer = []() {
  };
  std::mutex deviceStateHandlerMutex;
  std::string context_uri, context_url;
  void parseCommand(std::vector<uint8_t>& data);
  void skip(CommandType dir, bool notify);
  void runTask() override;

  void stopTask() { isRunning.store(false); }

  void unreference(char** s) {
    if (*s) {
      free(*s);
      *s = nullptr;
    }
  }

  static void reloadTrackList(void*);
  std::atomic<bool> resolvingContext = false;
};
}  // namespace spotify