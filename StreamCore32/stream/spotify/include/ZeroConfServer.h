#pragma once

#include <memory>
#include <string>
#include <atomic>

#include <LoginBlob.h>
#include "BellLogger.h"  // for setDefaultLogger, AbstractLogger
#include "Logger.h"
#include "nlohmann/json.hpp"
#include "ZeroConf.h"

using namespace spotify;
class ZeroconfAuthenticator {
public:
  ZeroconfAuthenticator(std::string name) {
    blob = std::make_shared<spotify::LoginBlob>(name);
  };
  ~ZeroconfAuthenticator() {};

  std::shared_ptr<spotify::LoginBlob> blob;

  std::atomic<bool> isRunning = false;

  std::function<void(std::shared_ptr<spotify::LoginBlob>)> onAuthSuccess;
  std::function<void()> onClose;

  void registerMdnsService() {

    this->isRunning.store(true);

    ZeroconfServiceManager::ServiceSpec spec;
    spec.key = "spotify";
    spec.serviceType = "_spotify-connect";   // same as before
    spec.proto = "_tcp";
    spec.instanceName = "StreamCore32";    // how it appears in mDNS
    spec.txt = {
      {"VERSION", "1.0"},
      {"CPath", "/spotify_info"},
      {"Stack", "SP"}
    };
    spec.endpoints.push_back({
      ZeroconfServiceManager::HttpMethod::GET,
      "/spotify_info",
      [this](mg_connection* c) -> std::string {
        return this->blob->buildZeroconfInfo();
      }
      });
    spec.endpoints.push_back({
      ZeroconfServiceManager::HttpMethod::POST,
      "/spotify_info",
      [this](mg_connection* c) -> std::string {
        nlohmann::json obj;
        // Prepare a success response for spotify
        obj["status"] = 101;
        obj["spotifyError"] = 0;
        obj["statusString"] = "ERROR-OK";

        std::string body = "";
        auto requestInfo = mg_get_request_info(c);
        if (requestInfo->content_length > 0) {
          body.resize(requestInfo->content_length);
          mg_read(c, body.data(), requestInfo->content_length);

          mg_header hd[10];
          int num = mg_split_form_urlencoded(body.data(), hd, 10);
          std::map<std::string, std::string> queryMap;

          // Parse the form data
          for (int i = 0; i < num; i++) {
            queryMap[hd[i].name] = hd[i].value;
          }

          SC32_LOG(info, "Received zeroauth POST data");

          // Pass user's credentials to the blob
          blob->loadZeroconfQuery(queryMap);
          onAuthSuccess(blob);

        }

        return obj.dump();
      }
      });
    spec.endpoints.push_back({
      ZeroconfServiceManager::HttpMethod::GET,
      "/close",
      [](mg_connection* c) -> std::string {
        return "";
      }
      });
    zeroconf.addService(spec);
  }
  void unregisterMdnsService() {
    this->isRunning.store(false);
    zeroconf.removeService("spotify");
  }

};