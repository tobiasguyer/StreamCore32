#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "BellTask.h"

#include "DeviceStateHandler.h"
#include "Logger.h"
#include "StreamBase.h"
#include "StreamCoreFile.h"
#include "ZeroConfServer.h"

/** TODO**
 * when spotify is running but not active, log 178 and 181
 */
class SpotifyStream : public StreamBase {
 private:
  std::unique_ptr<StreamCoreFile> creds;
  std::function<void(bool)> onConnect_ = nullptr;

 public:
  using OnUiMessage = std::function<void(const std::string&)>;
  using MetaCb = std::function<void(const std::string&, const std::string&)>;
  using ErrorCb = std::function<void(const std::string&)>;
  using StateCb = std::function<void(bool)>;
  std::shared_ptr<spotify::DeviceStateHandler> handler = nullptr;
  std::shared_ptr<ZeroconfAuthenticator> zeroconfServer;
  std::shared_ptr<spotify::Context> ctx;
  std::atomic<bool> isRunning = false;
  std::atomic<bool> isConnected = false;
  std::string currentUserName = "";
  SpotifyStream(std::shared_ptr<AudioControl> _audioController,
                std::unique_ptr<StreamCoreFile> _creds,
                std::function<void(bool)> _onConnect)
      : StreamBase("Spotify", _audioController, 1024 * 20, 1, 1, 1),
        creds(std::move(_creds)),
        onConnect_(_onConnect) {  // cannot run on PSRAM because of NVS
    SC32_LOG(info, "Starting SpotifyStream");
    feed_->state_callback = [this](uint8_t state) {
      if (handler == nullptr || !handler->is_active)
        return;
      if (handler->trackQueue == nullptr ||
          handler->trackQueue->preloadedTracks.size() == 0)
        return;

      std::shared_ptr<spotify::QueuedTrack> track =
          this->handler->trackQueue->preloadedTracks[0];
      switch (state) {
        case 1:
          track->trackMetrics->startTrackPlaying(track->requestedPosition);
          handler->putPlayerState();
          break;
        case 2:
          [[fallthrough]];
        case 3:
          if (onUiMessage_ && (state == 2 || state == 3)) {
            nlohmann::json j;
            j["type"] = "playback";
            j["src"] = "Spotify";
            switch (track->audioFormat) {
              case 0:
                j["quality"] = "Ogg Vorbis - 96 kbps";
                break;
              case 1:
                j["quality"] = "Ogg Vorbis - 160 kbps";
                break;
              case 2:
                j["quality"] = "Ogg Vorbis - 320 kbps";
                break;
              default:
                j["quality"] = "Unknown";
                break;
            }
            j["state"] = (int)(state == 2);
            j["position_ms"] = track->trackMetrics->getPosition(state == 3);
            j["duration_ms"] = track->trackInfo.duration;
            j["track"] = {{"title", track->trackInfo.name},
                          {"album", track->trackInfo.album},
                          {"artist", track->trackInfo.artist},
                          {"image", track->trackInfo.imageUrl}};
            onUiMessage_(j.dump());
          }
        default:
          break;
      }
    };
    // BELL_SLEEP_MS(1000); // wait for audio controller to be ready
    SC32_LOG(info, "Starting ZeroconfAuthenticator");
    this->zeroconfServer =
        std::make_shared<ZeroconfAuthenticator>(CONFIG_SPOTIFY_DEVICE_NAME);
    this->zeroconfServer->onClose = [this]() {
      if (this->isRunning) {
        if (this->handler->isRunning.load())
          this->handler.reset();
        this->isRunning.store(false);
        this->onStartup();
      }
    };
    zeroconfServer->onAuthSuccess =
        [this](std::shared_ptr<spotify::LoginBlob> blob) {
          isRunning.store(false);
          this->onAuthSuccess(blob);
        };
    SC32_LOG(info, "Starting Task");
    if (CONFIG_SPOTIFY_DISCOVERY_MODE_OPEN) {
      zeroconfServer->blob =
          std::make_shared<spotify::LoginBlob>(CONFIG_SPOTIFY_DEVICE_NAME);
      if (!zeroconfServer->isRunning.load())
        zeroconfServer->registerMdnsService();
    }
    onStartup();
  }

  void onLoginSuccess(std::shared_ptr<spotify::LoginBlob> blob) {
    Record r;
    r.userkey = blob->username;
    currentUserName = blob->username;
    r.fields.push_back(
        Field{"authType",
              std::vector<uint8_t>{static_cast<uint8_t>(blob->authType)}});
    r.fields.push_back(Field{"authData", blob->authData});
    creds->save(r, true);
    creds->set_current(r.userkey);
  }
  void stop() override {
    if (handler != nullptr) {
      handler->disconnect();
      handler.reset();
    }
  }
  void onClose(bool logout) {
    if (logout) {
      creds->erase(currentUserName);
      SC32_LOG(info, "Logout");
    }
    if (isRunning.load() == false)
      return;
    isRunning.store(false);
    onStartup();
  }
  void onAuthSuccess(std::shared_ptr<spotify::LoginBlob> blob) {
    onConnect_(true);
    zeroconfServer->blob = blob;
    startTask();
  }
  void runTask() override {
    //if(CONFIG_SPOTIFY_DISCOVERY_MODE_OPEN){
    if (handler != nullptr) {

      SC32_LOG(info, "Resetting handler");
      handler->disconnect();
      handler.reset();
      SC32_LOG(info, "Handler reset");
    }
    //}
    SC32_LOG(info, "Login success");
    if (!isRunning.load()) {
      isRunning.store(true);
      try {
        std::function<void(bool)> _onClose = [this](bool logout) {
          this->onClose(logout);
          onConnect_(false);
        };
        std::function<void()> _onTransfer = [this]() {
        };
        std::function<void(const std::string&, const std::vector<uint8_t>&)>
            _onLoginSuccess = [this](const std::string& username,
                                     const std::vector<uint8_t>& auth_data) {
              this->onLoginSuccess(std::make_shared<spotify::LoginBlob>(
                  CONFIG_SPOTIFY_DEVICE_NAME, username, auth_data));
            };
        std::function<uint16_t()> _getVolume = [this]() {
          return this->feed_->audioSink->get_logarithmic_volume<uint16_t>(
              this->audio_->volume);
        };
        handler = std::make_shared<spotify::DeviceStateHandler>(
            this->zeroconfServer->blob, _onClose, _onTransfer, _onLoginSuccess,
            _getVolume);
        if (!CONFIG_SPOTIFY_DISCOVERY_MODE_OPEN) {
          if (zeroconfServer->isRunning.load())
            zeroconfServer->unregisterMdnsService();
        }
        handler->trackPlayer->dataCallback =
            [fC = feed_](uint8_t* data, size_t bytes, size_t trackId,
                         bool VOLATILE) {
              return fC->feedData(data, bytes, trackId, VOLATILE);
            };
        handler->trackPlayer->headerSize = [aC = audio_](size_t trackId) {
          return aC->getHeaderOffset(trackId);
        };
        handler->stateToSinkCallback = [this](spotify::Command cmd) {
          auto map = [](spotify::CommandType t)
              -> std::optional<AudioControl::CommandType> {
            using CT = spotify::CommandType;
            using AC = AudioControl::CommandType;
            switch (t) {
              case CT::PLAY:
                return AC::PLAY;
              case CT::PAUSE:
                return AC::PAUSE;
              case CT::DISC:
                return AC::DISC;
              case CT::FLUSH:
                return AC::FLUSH;
              case CT::SKIP_NEXT:
              case CT::SKIP_PREV:
                return AC::SKIP;
              case CT::VOLUME:
                return AC::VOLUME_LOGARITHMIC;
              default:
                return std::nullopt;
                // e.g. PLAYBACK /* std::shared_ptr<spotify::QueuedTrack> track = std::get<std::shared_ptr<spotify::QueuedTrack>>(command.data);*/
            }
          };
          if (auto ac = map(cmd.commandType)) {
            uint16_t value = 0;
            if (*ac == AudioControl::CommandType::VOLUME_LOGARITHMIC) {
              if (auto p = std::get_if<int32_t>(&cmd.data))
                value = *p;
            }
            this->feed_->feedCommand(*ac, value);
          }
        };
        handler->ctx->session->startTask();
        handler->startTask();
      } catch (std::exception& e) {
        SC32_LOG(error, "Error while connecting %s", e.what());
        isRunning.store(false);
        return this->onClose(false);
      }
    }
  }

  void onStartup() {
    /*
    Record r;
    r.fields.push_back(Field{"authType", std::vector<uint8_t>()});
    r.fields.push_back(Field{"authData", std::vector<uint8_t>()});
    if (creds->get_current(&r) == 0) {
      SC32_LOG(info, "Found startup credentials");
      onAuthSuccess(std::make_shared<spotify::LoginBlob>(CONFIG_SPOTIFY_DEVICE_NAME,
                      r.userkey, r.fields[1].value));
    } else
    */
    if (zeroconfServer->isRunning.load() == false) {
      zeroconfServer->blob =
          std::make_shared<spotify::LoginBlob>(CONFIG_SPOTIFY_DEVICE_NAME);
      zeroconfServer->registerMdnsService();
    }
  }

  OnUiMessage onUiMessage_ = nullptr;
};