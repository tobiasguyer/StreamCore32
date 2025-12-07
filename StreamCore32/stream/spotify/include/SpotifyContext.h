#pragma once

#include <stdint.h>
#include <memory>

#include "Crypto.h"
#include "EventManager.h"
#include "LoginBlob.h"
#include "MercurySession.h"
#include "TimeProvider.h"
#include "protobuf/authentication.pb.h"  // for AuthenticationType_AUTHE...
#include "protobuf/metadata.pb.h"
#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/detail/json_pointer.hpp"  // for json_pointer<>::string_t
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif
#include "Logger.h"

#ifdef ESP_PLATFORM
#include "EspRandomEngine.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#else
#include <random>  //for random_device and default_random_engine
#endif

namespace spotify {
struct Context {
  struct ConfigState {
    // Setup default bitrate to 160
    AudioFormat audioFormat = (AudioFormat)CONFIG_SPOTIFY_AUDIO_FORMAT;
    std::string deviceId;
    std::string deviceName;
    std::vector<uint8_t> authData;
    std::function<uint16_t()> volume = []() {
      return 0xFFFF;
    };

#ifdef ESP_PLATFORM
    EventGroupHandle_t s_spotify_event_group;
#endif

    std::string username;
    std::string countryCode;
  };

  ConfigState config;

  std::shared_ptr<spotify::TimeProvider> timeProvider;
  std::shared_ptr<spotify::MercurySession> session;
  std::shared_ptr<PlaybackMetrics> playbackMetrics;

#ifdef ESP_PLATFORM
  streamcore::esp_random_device rd;
  streamcore::esp_random_engine rng;
#else
  std::random_device rd;
  std::default_random_engine rng;
#endif
  std::string getCredentialsJson() {
#ifdef BELL_ONLY_CJSON
    cJSON* json_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(json_obj, "authData",
                            Crypto::base64Encode(config.authData).c_str());
    cJSON_AddNumberToObject(
        json_obj, "authType",
        AuthenticationType_AUTHENTICATION_STORED_SPOTIFY_CREDENTIALS);
    cJSON_AddStringToObject(json_obj, "username", config.username.c_str());

    char* str = cJSON_PrintUnformatted(json_obj);
    cJSON_Delete(json_obj);
    std::string json_objStr(str);
    free(str);

    return json_objStr;
#else
    nlohmann::json obj;
    obj["authData"] = Crypto::base64Encode(config.authData);
    obj["authType"] =
        AuthenticationType_AUTHENTICATION_STORED_SPOTIFY_CREDENTIALS;
    obj["username"] = config.username;

    return obj.dump();
#endif
  }

  void lost_connection(void*) {
    //if(!connection)
  }

  static std::shared_ptr<Context> createFromBlob(
      std::shared_ptr<LoginBlob> blob) {
    auto ctx = std::make_shared<Context>();
    ctx->timeProvider = std::make_shared<TimeProvider>();
#ifdef ESP_PLATFORM
    ctx->rng = streamcore::esp_random_engine{};
#else
    ctx->rng = std::default_random_engine{ctx->rd()};
#endif
    ctx->session = std::make_shared<MercurySession>(ctx->timeProvider);
    ctx->playbackMetrics = std::make_shared<PlaybackMetrics>(ctx);
    ctx->config.deviceId = blob->getDeviceId();
    SC32_LOG(info, "Using device id %s", ctx->config.deviceId.c_str());
    ctx->config.deviceName = blob->getDeviceName();
    SC32_LOG(info, "Using device name %s", ctx->config.deviceName.c_str());
    ctx->config.authData = blob->authData;
    SC32_LOG(info, "Using auth data of size %d",
             (int)ctx->config.authData.size());
    ctx->config.username = blob->getUserName();
    SC32_LOG(info, "Using username %s", ctx->config.username.c_str());
    return ctx;
  }
};
}  // namespace spotify
