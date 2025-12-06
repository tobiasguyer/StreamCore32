#include "QobuzStream.h"
#include "HTTPClient.h"
#include "TLSSocket.h"
#include "URLParser.h"
#include <mbedtls/md5.h>   // available in ESP-IDF
#include <stdio.h>
#include <mbedtls/sha256.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#include "QobuzSign.h"
uint64_t current_session_id;
static std::string build_query(const std::vector<std::pair<std::string, std::string>>& kv) {
  std::string q;
  q.reserve(128);
  for (size_t i = 0;i < kv.size();++i) {
    if (i) q += '&';
    q += bell::URLParser::urlEncode(kv[i].first);
    q += '=';
    q += bell::URLParser::urlEncode(kv[i].second);
  } return q;
}

void QobuzStream::runTask() {
  if (cfg_.api_token.token.empty()) {
    if (!login()) {
      SC32_LOG(error, "Qobuz login failed");
      return;
    }
  }

  std::function<std::unique_ptr<bell::HTTPClient::Response>(
    const std::string& object, //session, user, file
    const std::string& action, // start, url...
    const std::vector<std::pair<std::string, std::string>>& params,
    bool sign
  )> onQobuzGet = [this](
    const std::string& object,
    const std::string& action,
    const std::vector<std::pair<std::string,
    std::string>>&params,
    bool sign) -> std::unique_ptr<bell::HTTPClient::Response> {
      uint64_t now_ms = timesync::now_ms();
      if ((uint64_t)cfg_.XsessionId.expiresAt < now_ms) startSession();
      bell::HTTPClient::Headers headers = {
        {"Referer", "https://play.qobuz.com/"},
        {"Origin", "https://play.qobuz.com"},
        {"X-App-Id", cfg_.appId},
        {"X-Session-Id", cfg_.XsessionId.token},
      };
      if (!cfg_.userAuthToken.empty()) {
        headers.push_back({ "X-User-Auth-Token", cfg_.userAuthToken });
      }
      else if (!cfg_.api_token.token.empty()) {
        headers.push_back({ "Authorization", "Bearer " + cfg_.api_token.token });
      }
      if (sign) return qobuzGet(cfg_.api_base, object,
        action, headers, params,
        timesync::now_s_text(6),
        cfg_.appSecret);
      else return qobuzGet(cfg_.api_base, object, action, headers, params);
    };
  std::function <std::unique_ptr<bell::HTTPClient::Response>(
    const std::string& object, //session, user, file
    const std::string& action, // start, url...
    const std::string& body,
    const std::vector<std::pair<std::string, std::string>>& params,
    const bool sign
  )> onQobuzPost = [this](
    const std::string& object,
    const std::string& action,
    const std::string& body,
    const std::vector<std::pair<std::string, std::string>>& params,
    bool sign) -> std::unique_ptr<bell::HTTPClient::Response> {
      uint64_t now_ms = timesync::now_ms();
      if ((uint64_t)cfg_.XsessionId.expiresAt < now_ms) startSession();
      bell::HTTPClient::Headers headers = {
        {"Referer", "https://play.qobuz.com/"},
        {"Origin", "https://play.qobuz.com"},
        {"X-App-Id", cfg_.appId},
        {"X-Session-Id", cfg_.XsessionId.token},
      };
      if (!cfg_.userAuthToken.empty()) {
        headers.push_back({ "X-User-Auth-Token", cfg_.userAuthToken });
      }
      else if (!cfg_.api_token.token.empty()) {
        headers.push_back({ "Authorization", "Bearer " + cfg_.api_token.token });
      }
      if (body.empty()) {
        if (sign) return qobuzPost(cfg_.api_base, object,
          action, headers, "", params,
          timesync::now_s_text(6), cfg_.appSecret);
        else return qobuzPost(cfg_.api_base, object, action, headers, "", params);
      }
      return qobuzPost(cfg_.api_base, object, action, headers, body, params);
    };

  queue_ = std::make_shared<qobuz::QobuzQueue>(cfg_.session_id.raw);
  queue_->onGet(onQobuzGet);
  queue_->onPost(onQobuzPost);
  queue_->onWsMessage([&](qconnect_QConnectMessage* args, size_t count) {
    encodeBatches(args, count);
    });

  player_ = std::make_shared<QobuzPlayer>(audioControl_, queue_);
  player_->onWsMessage([&](qconnect_QConnectMessage* args, size_t count) {
    encodeBatches(args, count);
    });
  player_->onGet(onQobuzGet);
  player_->onPost(onQobuzPost);
  player_->onUiMessage_ = [this](const std::string& msg) {
    onUiMessage_(msg);
    };

  if (cfg_.appSecret.empty() || !open("64868955")) {
    QobuzConfig::ClientAppSecrets secrets;
    config_ = std::make_unique<QobuzConfig>(&secrets);
    config_->loadedSemaphore->wait();
    cfg_.appId = secrets.id;

    for (auto& s : secrets.secrets) {
      cfg_.appSecret = s.second;
      if (open("64868955")) break;
    }
    config_.reset();
    Record appInfo;
    appInfo.userkey = "appInfo";
    appInfo.fields.push_back({ "appId", cfg_.appId });
    appInfo.fields.push_back({ "appSecret", cfg_.appSecret });
    creds->save(appInfo, true);
  }

  wsManager = std::make_unique<WsManager>([&]() { return getWSToken(); });
  wsManager->onAuth([this]() {
    WSRegisterController();
    });
  wsManager->onPayload([this](std::vector<uint8_t> data) {
    WSDecodePayload(std::move(data));
    });
  wsManager->startTask();
  queue_->startTask();
  token_hb_ = std::make_unique<Heartbeat>([&]() {
    if (cfg_.userAuthToken.empty()) {
      if (cfg_.api_token.expiresAt <= timesync::now_ms() + 60000) {
        refreshApiToken();
      }
    }
    else if (cfg_.XsessionId.expiresAt <= timesync::now_ms() + 60000) {
      startSession();
    }
    }, 30000);
  token_hb_->start();
}

bool QobuzStream::login() {
  if (cfg_.appId.empty()) return false;
  SC32_LOG(info, "Qobuz login: %s", cfg_.email.c_str());
  SC32_LOG(info, "Qobuz appId: %s", cfg_.appId.c_str());
  const std::string url = cfg_.api_base + "/user/login?" + build_query({
    {"email", cfg_.email}, {"password", cfg_.password}, {"app_id", cfg_.appId}
    });
  std::string msg = "extra=partner";

  std::unique_ptr<bell::HTTPClient::Response> qobuzResponse = std::make_unique<bell::HTTPClient::Response>();

  qobuzResponse->post(url, {}, std::vector<uint8_t>(msg.begin(), msg.end()));
  if (qobuzResponse->status() != 200) return false;

  auto body = qobuzResponse->body_string();

  auto json = nlohmann::json::parse(body);

  SC32_LOG(info, "Qobuz login response: %s", json.dump(2).c_str());
  cfg_.userAuthToken = json.at("user_auth_token").get<std::string>();
  if (json.find("user") == json.end())
    SC32_LOG(error, "Qobuz login failed: %s", json.dump(2).c_str());
  if (json["user"].find("id") == json["user"].end())
    SC32_LOG(error, "Qobuz get user failed: %s", json.dump(2).c_str());
  cfg_.userId = std::to_string(json["user"]["id"].get<int>());
  player_->user_id_ = cfg_.userId;
  SC32_LOG(info, "Qobuz login finished with app_id: %s and user_auth_token: %s", cfg_.appId.c_str(), cfg_.userAuthToken.c_str());
  return !cfg_.userAuthToken.empty();
}

bool QobuzStream::refreshApiToken() {
  HTTPClient::Headers headers = {
    {"Referer", "https://play.qobuz.com/"},
    {"Content-Type", "application/x-www-form-urlencoded"},
    {"Origin", "https://play.qobuz.com"},
    {"X-App-Id", cfg_.appId},
    {"X-Session-Id", cfg_.XsessionId.token},
    { "Authorization", "Bearer " + cfg_.api_token.token },
  };
  auto resp = qobuzPost(cfg_.api_base, "qws", "refreshToken", headers, "jwt=jwt_api");
  if (resp->status() == 200) {
    std::string body = resp->body_string();
    SC32_LOG(info, "Qobuz token: %s", body.c_str());
    nlohmann::json j = nlohmann::json::parse(body);
    cfg_.api_token.token = j["jwt_api"]["jwt"];
    cfg_.api_token.expiresAt = j["jwt_api"]["exp"].get<uint64_t>() * 1000;
  }
  else {
    SC32_LOG(info, "Qobuz token: %s", resp->body_string().c_str());
    return false;
  }
  return true;
}


std::unique_ptr<bell::HTTPClient::Response> QobuzStream::qobuzGet(
  const std::string& url_base,
  const std::string& object, //session, user, file
  const std::string& action, // start, url...
  const std::vector<std::pair<std::string, std::string>>& headers,
  const std::vector<std::pair<std::string, std::string>>& params,
  const std::string& request_ts,
  const std::string& app_secret
) {
  auto url = url_base + "/" + object;
  if (action.size()) url += "/" + action;
  if (params.size()) {
    url += "?" + build_query(params);
    if (request_ts.size()) url += "&request_ts=" + request_ts;
    if (app_secret.size()) {
      const std::string sig = qobuz::md5_sig(object, action, params, request_ts, app_secret);
      url += "&request_sig=" + sig;
    }
  }
  std::unique_ptr<bell::HTTPClient::Response> qobuzResponse = std::make_unique<bell::HTTPClient::Response>();
  qobuzResponse->get(url, headers, false);
  return qobuzResponse;
}
std::unique_ptr<bell::HTTPClient::Response> QobuzStream::qobuzPost(
  const std::string& url_base,
  const std::string& object,
  const std::string& action,
  const std::vector<std::pair<std::string, std::string>>& headers,
  const std::string& body,
  const std::vector<std::pair<std::string, std::string>>& params,
  const std::string& request_ts,
  const std::string& app_secret
) {
  auto url = url_base + "/" + object;
  if (action.size()) url += "/" + action;
  std::vector<uint8_t> body_ = {};
  if (body.size())
    body_ = std::vector<uint8_t>(body.begin(), body.end());
  else {
    std::string temp = build_query(params);
    if (request_ts.size()) temp += "&request_ts=" + request_ts;
    if (app_secret.size()) {
      const std::string sig = qobuz::md5_sig(object, action, params, request_ts, app_secret);
      temp += "&request_sig=" + sig;
    }
    body_ = std::vector<uint8_t>(temp.begin(), temp.end());
  }
  std::string body_str = std::string(body_.begin(), body_.end());
  std::unique_ptr<bell::HTTPClient::Response> qobuzResponse = std::make_unique<bell::HTTPClient::Response>();
  qobuzResponse->post(url, headers, body_);
  return qobuzResponse;
}

WSToken QobuzStream::getWSToken() {
  WSToken token = cfg_.ws_token;
  if (cfg_.userAuthToken.empty() && token.jwt.empty()) return token;
  HTTPClient::Headers headers = {
    {"Referer", "https://play.qobuz.com/"},
    {"Content-Type", "application/x-www-form-urlencoded"},
    {"Origin", "https://play.qobuz.com"},
    {"X-App-Id", cfg_.appId},
    {"X-Session-Id", cfg_.XsessionId.token},
  };
  if (!cfg_.userAuthToken.empty()) {
    headers.push_back({ "X-User-Auth-Token", cfg_.userAuthToken });
  }
  else if (!cfg_.api_token.token.empty()) {
    headers.push_back({ "Authorization", "Bearer " + cfg_.api_token.token });
  }
  std::string endpoint;
  if (cfg_.ws_token.jwt.empty()) endpoint = "createToken";
  else endpoint = "refreshToken";
  auto resp = qobuzPost(cfg_.api_base, "qws", endpoint, headers, "jwt=jwt_qws");
  if (resp->status() == 200) {
    std::string body = resp->body_string();
    nlohmann::json j = nlohmann::json::parse(body);
    token.jwt = j["jwt_qws"]["jwt"];
    token.exp_s = j["jwt_qws"]["exp"];
    token.endpoint = URLParser::urlDecode(j["jwt_qws"]["endpoint"]);
  }
  else {
    SC32_LOG(info, "Qobuz token: %s", resp->body_string().c_str());
  }

  return token;
}

bool QobuzStream::startSession() {
  if (cfg_.userAuthToken.empty() && cfg_.api_token.token.empty()) return false;
  timesync::wait_until_valid(8000);

  const std::string ts_text = timesync::now_s_text(6);

  const std::vector<std::pair<std::string, std::string>> params = {
    {"profile","qbz-1"}
  };

  const std::string app_secret = qobuz::maybe_unpack_secret(cfg_.appSecret);
  std::string endpoint = "start";
  const std::string sig = qobuz::md5_sig("session", endpoint, params, ts_text, app_secret);

  const std::string body =
    qobuz::build_query(params) +
    "&request_ts=" + ts_text +
    "&request_sig=" + sig;
  HTTPClient::Headers headers = {
    {"Referer", "https://play.qobuz.com/"},
    {"Content-Type", "application/x-www-form-urlencoded"},
    {"Origin", "https://play.qobuz.com"},
    {"X-App-Id", cfg_.appId},
  };
  if (!cfg_.userAuthToken.empty()) {
    headers.push_back({ "X-User-Auth-Token", cfg_.userAuthToken });
  }
  else if (!cfg_.api_token.token.empty()) {
    headers.push_back({ "Authorization", "Bearer " + cfg_.api_token.token });
  }
  auto resp = qobuzPost(cfg_.api_base, "session", endpoint, headers, body);
  if (resp->status() != 200) {
    SC32_LOG(info, "Qobuz start: %s", resp->body_string().c_str());
    return false;
  }
  else {
    nlohmann::json j = nlohmann::json::parse(resp->body_string());
    cfg_.XsessionId.token = j["session_id"];
    cfg_.XsessionId.expiresAt = j["expires_at"];
    SC32_LOG(info, "Qobuz start: expires_at: %llu", cfg_.XsessionId.expiresAt);
    cfg_.XsessionId.expiresAt *= 1000;
    SC32_LOG(info, "Qobuz start: session_id: %s", cfg_.XsessionId.token.c_str());
    SC32_LOG(info, "Qobuz start: expires_at: %llu", cfg_.XsessionId.expiresAt);
    cfg_.infos = j["infos"];

    SC32_LOG(info, "Qobuz start: session_id: %s", cfg_.XsessionId.token.c_str());
    SC32_LOG(info, "Qobuz start: infos: %s", cfg_.infos.c_str());
  }
  return true;
}

bool QobuzStream::open(const std::string& trackId) {
  if (cfg_.appSecret.empty()) return false;
  timesync::wait_until_valid(8000);
  const std::string ts_text = timesync::now_s_text(6);

  const std::vector<std::pair<std::string, std::string>> params = {
    {"format_id", "5"},
    {"intent", "stream"},
    {"track_id", trackId}
  };
  std::vector<std::pair<std::string, std::string>> headers = {
    {"Referer", "https://play.qobuz.com/"},
    {"Origin", "https://play.qobuz.com"},
    {"X-App-Id", cfg_.appId},
  };
  if (!cfg_.userAuthToken.empty()) {
    headers.push_back({ "X-User-Auth-Token", cfg_.userAuthToken });
  }
  else if (!cfg_.api_token.token.empty()) {
    headers.push_back({ "Authorization", "Bearer " + cfg_.api_token.token });
  }

  auto resp = qobuzGet(cfg_.api_base, "track", "getFileUrl", headers, params, ts_text, cfg_.appSecret);
  if (resp->status() != 200) {
    SC32_LOG(info, "Qobuz open: %s", resp->body_string().c_str());
    SC32_LOG(info, "Qobuz open: %d", resp->status());
    return false;
  }
  auto body = resp->body_string();
  nlohmann::json j = nlohmann::json::parse(body);
  if (j.contains("status") && j["status"] == "error") return false;
  return true;
}

void QobuzStream::WSRegisterController() {
  qconnect_QConnectMessage device = qconnect_QConnectMessage_init_default;
  device.messageType = qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_JOIN_SESSION;
  device.has_messageType = true;
  device.has_ctrlSrvrJoinSession = true;
  device.ctrlSrvrJoinSession.has_deviceInfo = true;
  device.ctrlSrvrJoinSession.deviceInfo.deviceUuid = vectorToPbArray(
    std::vector<uint8_t>(cfg_.session_id.raw, cfg_.session_id.raw + 16)
  );
  device.ctrlSrvrJoinSession.deviceInfo.friendlyName = strdup(cfg_.name.c_str());
  device.ctrlSrvrJoinSession.deviceInfo.has_type = true;
  device.ctrlSrvrJoinSession.deviceInfo.type = qconnect_DeviceType_DEVICE_TYPE_SPEAKER;
  device.ctrlSrvrJoinSession.deviceInfo.has_capabilities = true;
  device.ctrlSrvrJoinSession.deviceInfo.capabilities = { 1, 1, 1, 4, 1, 2 };
  device.ctrlSrvrJoinSession.deviceInfo.softwareVersion = strndup("sc32-1.0.0", strlen("sc32-1.0.0"));//"sc32-1.0.0";
  this->encodeBatches(&device, 1);
}

void QobuzStream::WSDecodePayload(std::vector<uint8_t> data) {

  qconnect_Payload res = qconnect_Payload_init_default;
  if (!pbDecode(res, qconnect_Payload_fields, data)) {
    SC32_LOG(error, "Failed to decode payload");
    for (int i = 0; i < data.size(); i++) {
      printf("%02x ", data[i]);
    }
    printf("\n");
  }

  if (res.payload != NULL) {
    qconnect_QConnectBatch batch = qconnect_QConnectBatch_init_default;
    if (!pbDecode(batch, qconnect_QConnectBatch_fields, res.payload)) {
      SC32_LOG(error, "Failed to decode batch");
      for (int i = 0; i < data.size(); i++) {
        printf("%02x ", data[i]);
      }
      printf("\n");
    }
    for (int i = 0; i < batch.messages_count; i++) {
      WSDecodeMessage(&batch.messages[i]);
    }
    pb_release(qconnect_QConnectBatch_fields, &batch);
  }
  pb_release(qconnect_Payload_fields, &res);
}
uint64_t current_missmatch = 0;
void QobuzStream::WSDecodeMessage(_qconnect_QConnectMessage* data) {
  switch (data->messageType)
  {
  case qconnect_QConnectMessageType_MESSAGE_TYPE_ERROR:
    SC32_LOG(error, "Error message received");
    SC32_LOG(error, "Error code: %s", data->error.code);
    SC32_LOG(error, "Error description: %s", data->error.message);
    if (strcmp(data->error.message, "Current track not found in queue nor autoplay") == 0) {
      if (current_missmatch != player_->getCurrentTrack()->id) {
        current_missmatch = player_->getCurrentTrack()->id;
        player_->setTracks();
      }
      else {
        player_->stopTrack();
      }
    }
    break;
  case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_QUEUE_ERROR_MESSAGE: {
    if (!data->has_srvrCtrlQueueErrorMessage) break;
    SC32_LOG(error, "Queue error message received");
    SC32_LOG(error, "Error code: %s", data->srvrCtrlQueueErrorMessage.error.code);
    SC32_LOG(error, "Error description: %s", data->srvrCtrlQueueErrorMessage.error.message);
    if (strcmp(data->srvrCtrlQueueErrorMessage.error.message, "Queue version mismatch") == 0) {
      queue_->queueuState.queueVersion = data->srvrCtrlQueueErrorMessage.queueVersion;
      queue_->getSuggestions();
    }
    break;
  }
  case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_RNDR_SET_ACTIVE: {
    break;
  }
  case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_ADD_RENDERER: {
    if (!data->has_srvrCtrlAddRenderer) break;
    if (std::equal(
      data->srvrCtrlAddRenderer.renderer.deviceUuid->bytes,
      data->srvrCtrlAddRenderer.renderer.deviceUuid->bytes +
      data->srvrCtrlAddRenderer.renderer.deviceUuid->size,
      cfg_.session_id.raw)) {
      SC32_LOG(info, "RendererId %llu", data->srvrCtrlAddRenderer.rendererId);
      cfg_.rendererId = data->srvrCtrlAddRenderer.rendererId;
      if (isActive_.load()) {
        WSSetRendererActive();
        WSSetRendererVolume();
      }
    }
    break;
  }
  case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_SESSION_STATE: {
    if (!data->has_srvrCtrlSessionState) break;
    queue_->queueuState.queueVersion = data->srvrCtrlSessionState.queueVersion;
    current_session_id = data->srvrCtrlSessionState.sessionId;
    WSAskForQueueState();
    WSAskForRendererState();
    break;
  }
  case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_ACTIVE_RENDERER_CHANGED: {
    if (!data->has_srvrCtrlActiveRendererChanged) break;
    SC32_LOG(info, "Active renderer changed to %llu", data->srvrCtrlActiveRendererChanged.rendererId);
    if (data->srvrCtrlActiveRendererChanged.rendererId == cfg_.rendererId) {
      if (!player_->isRunning()) {
        if (!isActive_.load()) {
          WSSetRendererActive();
          WSSetRendererVolume();
          this->player_->startTask();
        }
        isActive_.store(true);

      }
    }
    else if (player_->isRunning()) {
      stop();
    }
    break;
  }
  case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_QUEUE_STATE: {
    if (!data->has_srvrCtrlQueueState) break;
    queue_->consumeQueueState(data->srvrCtrlQueueState);
    break;
  case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_SHUFFLE_MODE_SET:
    if (data->has_srvrCtrlShuffleModeSet) {
      WSAskForQueueState();
      WSAskForRendererState();
    }
    else {
      SC32_LOG(info, "Shuffle mode set message without content");
      queue_->addShuffleIndexes(queue_->getRegularTracksSize());
      if (player_->isRunning()) {
        queue_->setIndex(player_->getCurrentTrack()->index);
      }
    }
    break;
  }
  case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_QUEUE_TRACKS_LOADED: {
    if (!data->has_srvrCtrlQueueTracksLoaded) break;
    SC32_LOG(info, "Queue tracks loaded - Queue version %llu/%llu", data->srvrCtrlQueueTracksLoaded.queueVersion.major, data->srvrCtrlQueueTracksLoaded.queueVersion.minor);
    queue_->deleteQobuzTracks();
    queue_->queueuState.queueVersion = data->srvrCtrlQueueTracksLoaded.queueVersion;
    WSAskForQueueState();
    WSAskForRendererState();
    queue_->addQobuzTracks(
      data->srvrCtrlQueueTracksLoaded.tracks,
      data->srvrCtrlQueueTracksLoaded.tracks_count,
      std::nullopt,
      data->srvrCtrlQueueTracksLoaded.contextUuid
    );
    queue_->addShuffleIndexes();
    if (player_->isRunning()) {
      resetPlayer_.store(true);
    }
    break;
  }
  case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_QUEUE_TRACKS_INSERTED: {
    if (!data->has_srvrCtrlQueueTracksInserted) break;
    if (data->srvrCtrlQueueTracksInserted.autoplayReset) {
      queue_->deleteAutoplayTracks();
    }
    size_t insertIndex;
    queue_->position(insertIndex, data->srvrCtrlQueueTracksInserted.insertAfter);
    queue_->addQobuzTracks(
      data->srvrCtrlQueueTracksInserted.tracks,
      data->srvrCtrlQueueTracksInserted.tracks_count,
      insertIndex,
      data->srvrCtrlQueueTracksInserted.contextUuid
    );
    queue_->addShuffleIndexes(data->srvrCtrlQueueTracksInserted.tracks_count, insertIndex);
    break;
  }
  case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_QUEUE_TRACKS_ADDED: {
    if (!data->has_srvrCtrlQueueTracksAdded) break;
    if (data->srvrCtrlQueueTracksAdded.autoplayReset) {
      queue_->deleteAutoplayTracks();
    }
    queue_->addQobuzTracks(
      data->srvrCtrlQueueTracksAdded.tracks,
      data->srvrCtrlQueueTracksAdded.tracks_count,
      std::nullopt,
      data->srvrCtrlQueueTracksAdded.contextUuid
    );
    queue_->addShuffleIndexes(queue_->getRegularTracksSize() + data->srvrCtrlQueueTracksAdded.tracks_count);
    break;
  }
  case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_QUEUE_TRACKS_REMOVED: {
    if (!data->has_srvrCtrlQueueTracksRemoved) break;
    queue_->deleteQobuzTracks(
      data->srvrCtrlQueueTracksRemoved.queueItemIds,
      data->srvrCtrlQueueTracksRemoved.queueItemIds_count
    );
    break;
  }
  case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_AUTOPLAY_TRACKS_LOADED: {
    if (!data->has_srvrCtrlAutoplayTracksLoaded) break;
    if (sentLoadedTracks_.load()) {
      sentLoadedTracks_.store(false);
      queue_->updateQobuzTracks(
        data->srvrCtrlAutoplayTracksLoaded.tracks,
        data->srvrCtrlAutoplayTracksLoaded.tracks_count,
        data->srvrCtrlAutoplayTracksLoaded.contextUuid
      );
      break;
    }
    queue_->deleteAutoplayTracks();
    queue_->addQobuzTracks(
      data->srvrCtrlAutoplayTracksLoaded.tracks,
      data->srvrCtrlAutoplayTracksLoaded.tracks_count,
      std::nullopt,
      data->srvrCtrlAutoplayTracksLoaded.contextUuid
    );
    break;
  }
  case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_AUTOPLAY_TRACKS_REMOVED: {
    if (!data->has_srvrCtrlAutoplayTracksRemoved) break;
    queue_->deleteQobuzTracks(
      data->srvrCtrlAutoplayTracksRemoved.queueItemIds,
      data->srvrCtrlAutoplayTracksRemoved.queueItemIds_count
    );
    break;
  }
  case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_VOLUME_CHANGED: {
    if (!data->has_srvrCtrlVolumeChanged) break;
    if (data->srvrCtrlVolumeChanged.rendererId == cfg_.rendererId) {
      player_->feed_->feedCommand(
        AudioControl::VOLUME_LINEAR,
        data->srvrCtrlVolumeChanged.volume,
        std::optional<uint32_t>(100) // max volume
      );
    }
    break;
  }
  case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_RNDR_SET_VOLUME: {
    if (!data->has_srvrRndrSetVolume) break;
    if (player_->isRunning()) {
      player_->feed_->feedCommand(
        AudioControl::VOLUME_LINEAR,
        data->srvrRndrSetVolume.volume,
        std::optional<uint32_t>(100) // max volume
      );
    }
    break;
  }
  case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_RENDERER_STATE_UPDATED: {
    if (!data->has_srvrCtrlRendererStateUpdated) break;
    if (data->srvrCtrlRendererStateUpdated.rendererId == cfg_.rendererId) {
      if (!player_->isRunning()) {
        queue_->setIndex(data->srvrCtrlRendererStateUpdated.state.currentQueueIndex);
        queue_->setStartAt(data->srvrCtrlRendererStateUpdated.state.currentPosition.value);
        this->player_->startTask();
      }
      //queue_->queueuState.queueVersion = data->srvrCtrlRendererStateUpdated.state.
    }
    else {
      queue_->setIndex(data->srvrCtrlRendererStateUpdated.state.currentQueueIndex);
      queue_->setStartAt(data->srvrCtrlRendererStateUpdated.state.currentPosition.value);
    }
    break;
  }
  case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_RNDR_SET_STATE: {
    if (!data->has_srvrRndrSetState) break;
    qconnect_SrvrRndrSetState* state = &data->srvrRndrSetState;
    if (state->has_queueVersion) {
      queue_->queueuState.queueVersion = state->queueVersion;
    }
    if (player_->isRunning() || isActive_.load()) {
      if (state->has_currentPosition && !state->has_currentQueueItem) {
        player_->requestSkipTo(state->currentPosition);
      }
      else if (state->has_currentQueueItem) {
        auto currentTrack = player_->getCurrentTrack();
        if (
          currentTrack &&
          currentTrack->index == state->nextQueueItem.queueItemId &&
          player_->currentTrackValueMs() >= 3000
          ) { //if more than 3s just restart
          player_->requestSkipTo(0);
          break;
        }
        else if (currentTrack && currentTrack->index == state->currentQueueItem.queueItemId) {
          SC32_LOG(info, "current track %lu", state->currentQueueItem.queueItemId);
          queue_->setIndex(state->currentQueueItem);
          break;
        }
        else {
          SC32_LOG(info, "current track %lu", state->currentQueueItem.queueItemId);
        }
        queue_->setIndex(state->currentQueueItem);
        player_->stopTrack();
      }
      else if (state->playingState == qconnect_PlayingState_PLAYING_STATE_PAUSED) {
        player_->feed_->feedCommand(AudioControl::PAUSE, 0);
        player_->player_state.playingState = qconnect_PlayingState_PLAYING_STATE_PAUSED;
      }
      else if (state->playingState == qconnect_PlayingState_PLAYING_STATE_PLAYING) {
        player_->feed_->feedCommand(AudioControl::PLAY, 0);
        player_->player_state.playingState = qconnect_PlayingState_PLAYING_STATE_PLAYING;
      }
    }
    if (isActive_.load() && !player_->isRunning()) {
      queue_->setIndex(state->currentQueueItem);
      queue_->setStartAt(state->currentPosition);
      this->player_->startTask();
    }
    break;
  }
  case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_LOOP_MODE_SET: {
    if (!data->has_srvrCtrlLoopModeSet) break;
    if (data->srvrCtrlLoopModeSet.mode == qconnect_LoopMode_LOOP_MODE_OFF) {
      queue_->setRepeat(false);
      player_->setRepeatOne(false);
    }
    else if (data->srvrCtrlLoopModeSet.mode == qconnect_LoopMode_LOOP_MODE_REPEAT_ONE) {
      queue_->setRepeat(false);
      player_->setRepeatOne(true);
    }
    else {
      queue_->setRepeat(true);
      player_->setRepeatOne(false);
    }
    break;
  }
  case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_QUEUE_VERSION_CHANGED: {
    if (!data->has_srvrCtrlQueueVersionChanged) break;
    queue_->queueuState.queueVersion = data->srvrCtrlQueueVersionChanged.queueVersion;
    break;
  }
  default:
    SC32_LOG(info, "UNKNOWN PAYLOAD %d", data->messageType);
    break;
  }
}

void QobuzStream::WSSetRendererActive() {
  qconnect_QConnectMessage msg = qconnect_QConnectMessage_init_default;
  msg.messageType = qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_SET_ACTIVE_RENDERER;
  msg.has_messageType = true;
  msg.has_ctrlSrvrSetActiveRenderer = true;
  msg.ctrlSrvrSetActiveRenderer.has_rendererId = true;
  msg.ctrlSrvrSetActiveRenderer.rendererId = cfg_.rendererId;
  this->encodeBatches(&msg, 1);
}

void QobuzStream::WSAskForQueueState() {
  qconnect_QConnectMessage msg = qconnect_QConnectMessage_init_default;
  msg.messageType = qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_ASK_FOR_QUEUE_STATE;
  msg.has_messageType = true;
  msg.has_ctrlSrvrAskForQueueState = true;
  msg.ctrlSrvrAskForQueueState.has_queueVersion = true;
  msg.ctrlSrvrAskForQueueState.queueVersion = queue_->queueuState.queueVersion;
  msg.ctrlSrvrAskForQueueState.queueUuid = static_cast<pb_bytes_array_t*>(malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(16)));//(&cfg_.queue_uuid);
  msg.ctrlSrvrAskForQueueState.queueUuid->size = 16;
  memcpy(msg.ctrlSrvrAskForQueueState.queueUuid->bytes, cfg_.session_id.raw, 16);
  this->encodeBatches(&msg, 1);
}
void QobuzStream::WSAskForRendererState() {
  qconnect_QConnectMessage msg = qconnect_QConnectMessage_init_default;
  msg.messageType = qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_ASK_FOR_RENDERER_STATE;
  msg.has_messageType = true;
  msg.has_ctrlSrvrAskForRendererState = true;
  msg.ctrlSrvrAskForRendererState.has_sessionId = true;
  msg.ctrlSrvrAskForRendererState.sessionId = current_session_id;
  this->encodeBatches(&msg, 1);
}

void QobuzStream::WSSetRendererVolume() {
  qconnect_QConnectMessage msg = qconnect_QConnectMessage_init_default;
  msg.messageType = qconnect_QConnectMessageType_MESSAGE_TYPE_RNDR_SRVR_VOLUME_CHANGED;
  msg.has_messageType = true;
  msg.has_rndrSrvrVolumeChanged = true;
  msg.rndrSrvrVolumeChanged.has_volume = true;
  msg.rndrSrvrVolumeChanged.volume = player_->audio_->volume.load();
  this->encodeBatches(&msg, 1);
}