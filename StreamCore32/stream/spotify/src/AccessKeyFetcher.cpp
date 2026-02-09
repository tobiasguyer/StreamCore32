#include "AccessKeyFetcher.h"

#include <cstring>           // for strrchr
#include <initializer_list>  // for initializer_list
#include <map>               // for operator!=, operator==
#include <type_traits>       // for remove_extent_t
#include <vector>            // for vector

#include "BellLogger.h"  // for AbstractLogger
#include "HTTPClient.h"
#include "Logger.h"          // for SC32_LOG
#include "Packet.h"          // for spotify
#include "SpotifyContext.h"  // for Context
#include "TimeProvider.h"    // for TimeProvider
#include "Utils.h"           // for string_format

#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif

using namespace spotify;

static std::string SCOPES =
    "streaming,user-library-read,user-library-modify,user-top-read,user-read-"
    "recently-played";  // Required access scopes

AccessKeyFetcher::AccessKeyFetcher(std::shared_ptr<spotify::Context> ctx)
    : ctx(ctx) {}

bool AccessKeyFetcher::isExpired() {
  if (accessKey.empty()) {
    return true;
  }

  if (ctx->timeProvider->getSyncedTimestamp() > expiresAt) {
    return true;
  }

  return false;
}

std::string AccessKeyFetcher::getAccessKey() {
  if (!isExpired()) {
    return accessKey;
  }

  updateAccessKey();

  return accessKey;
}

void AccessKeyFetcher::updateAccessKey() {
  if (keyPending) {
    // Already pending refresh request
    return;
  }

  keyPending = true;

  // Max retry of 3, can receive different hash cat types
  int retryCount = 3;
  bool success = false;

  do {
    std::string credentials;

    credentials =
        "grant_type=client_credentials&client_id=" + ctx->config.clientId +
        "&client_secret=" + ctx->config.clientSecret + "&scope=" + SCOPES;
    std::vector<uint8_t> body(credentials.begin(), credentials.end());

    auto response = bell::HTTPClient::post(
        "https://accounts.spotify.com/api/token",
        {{"Content-Type", "application/x-www-form-urlencoded"}}, body);

    auto responseBytes = response->bytes();
    auto root = nlohmann::json::parse(responseBytes);
    if (!root.contains("error")) {
      success = true;
      accessKey = std::string(root["access_token"]);
      int expiresIn = root["expires_in"];
      expiresAt =
          ctx->timeProvider->getSyncedTimestamp() + ((expiresIn * 1000) / 2);
    }
    retryCount--;
  } while (retryCount >= 0 && !success);
  keyPending = false;
}
