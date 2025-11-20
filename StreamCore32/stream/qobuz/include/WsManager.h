#pragma once

#include <string>
#include <vector>
#include <map>

#include "BellTask.h"
#include "WebSocketClient.h"

#include "protobuf/qconnect_envelope.pb.h"
#include "protobuf/qconnect_payload.pb.h"
#include "NanoPBHelper.h"

struct WSToken {
  std::string jwt;
  uint64_t    exp_s = 0;        // UTC seconds
  std::string endpoint;
};

class WsManager : public bell::Task
{
public:

  using OnOpen = std::function<void()>;

  using Callback = std::function<void(const std::string& payload)>;
  using BytesCallback = std::function<void(const std::vector<uint8_t>& payload)>;
  using TokenRefresher = std::function<WSToken()>;

  WsManager(TokenRefresher refresh_token) : bell::Task("WsManager", 4096 * 4, 1, 1) {
    this->refresh_token_ = std::move(refresh_token);
    if (refresh_token_) {
      auto t = refresh_token_();
      if (!t.jwt.empty()) {
        setToken(t);
      }
    }
    client_ = std::make_unique<WebSocketClient>();
    client_->onOpen([this]() {

      this->auth();
      this->tx_lock_.store(false);

      this->send(qconnect_QCloudMessageType_SUBSCRIBE);

      this->on_auth_();
      });
    client_->onClose([&](int code, const std::string& reason) {
      BELL_LOG(error, "qws", "CLOSED %d %s", code, reason.c_str());
      });
  };
  ~WsManager() {
    client_->close();
    isRunning_.store(false);
    std::scoped_lock lock(isRunningMutex_);

  };
  void setToken(const WSToken& t) {
    token_ = t.jwt;
    token_exp_s_ = t.exp_s;
    if (!t.endpoint.empty()) endpoint_ = t.endpoint;
  }
  void onAuth(OnOpen f) { on_auth_ = std::move(f); }
  void onPayload(BytesCallback f) { on_payload_ = std::move(f); }
  void auth() {
    if (token_.empty()) return;
    uint64_t ts = timesync::now_ms();
    qconnect_Authenticate auth = {
      1, ++next_id_,
      1, ts,
      strdup(token_.c_str())
    };
    BELL_LOG(info, "qws", "AUTH %s", token_.c_str());
    auto data = client_->pack(qconnect_QCloudMessageType_AUTHENTICATE, pbEncode(qconnect_Authenticate_fields, &auth));
    client_->send(0x2, data);
    pb_release(qconnect_Authenticate_fields, &auth);
  }
  void onConnected(){
    tx_lock_.store(false);
  }
  void send(uint8_t msg_type, const std::vector<uint8_t>& payload = {}, const std::vector<std::vector<uint8_t>>& dests = {}, uint64_t ts = 0, Callback cb = nullptr) {
    while(tx_lock_.load()) BELL_SLEEP_MS(100);
    tx_lock_.store(true);
    uint32_t id = ++next_id_;
    if (!ts) ts = timesync::now_ms();
    if (cb) {
      std::scoped_lock lock(cb_mtx_);
      cbs_[id] = std::move(cb);
    }
    qconnect_Payload env = qconnect_Payload_init_zero;
    env.has_msgId = true;
    env.msgId = id;
    env.has_msgDate = true;
    env.msgDate = ts;
    env.has_proto = true;
    env.proto = qconnect_QCloudProto_QP_QCONNECT;
    if (dests.size()) {
      env.dests = (pb_bytes_array_t**)calloc(dests.size(), sizeof(pb_bytes_array_t*));
      env.dests_count = dests.size();
      for (int i = 0; i < dests.size(); i++) {
        env.dests[i] = vectorToPbArray(dests[i]);
      }
    }
    if (payload.size()) {
      env.payload = vectorToPbArray(payload);
    }
    auto data = client_->pack(msg_type, pbEncode(qconnect_Payload_fields, &env));
    client_->send(0x2, data);
    pb_release(qconnect_Payload_fields, &env);
    last_tx_ms_ = ts;
    tx_lock_.store(false);
  }


  void runTask() override {
    std::scoped_lock lock(isRunningMutex_);
    isRunning_.store(true);
    const uint64_t REFRESH_WINDOW_MS = 60 * 1000; // refresh 60s before expiry

    while (isRunning_.load()) {
      if (!client_->connect(endpoint_, "https://play.qobuz.com", { "qws" })) {
        BELL_LOG(error, "qws", "connect failed; retrying in 2s");
        BELL_SLEEP_MS(2000);
        continue;
      }

      // OPTIONAL: tune keepalive
      client_->setKeepalive(/*pingEveryMs*/30000, /*pongTimeoutMs*/30000);

      client_->startTask(); // starts WS I/O loop
      while (isRunning_.load() && client_->isOpen()) {
        auto data = client_->handleFrame();
        if (data.size()) {
          auto resp = client_->parse(data);
          if (resp.size()) {
            for (auto& r : resp) {
              std::scoped_lock lock(cb_mtx_);
              auto it = cbs_.find(r.first);
              if (it != cbs_.end()) {
                //it->second(r.second);
                cbs_.erase(it);
              }
              else if (r.first == 6) {
                on_payload_(r.second);
              }
              else {
                BELL_LOG(error, "qws", "no callback for command %d", r.first);
                for (int i = 0; i < r.second.size(); i++) {
                  printf("%02x ", r.second[i]);
                }
                printf("\n");
              }
            }
          }

        }
        BELL_SLEEP_MS(100);
        if (token_exp_s_) {
          uint64_t now = timesync::now_ms(); // UTC ms
          if (now - last_tx_ms_ > REFRESH_WINDOW_MS && 
            now + REFRESH_WINDOW_MS >= token_exp_s_ * 1000ULL) {
            if (refresh_token_) {
              while(tx_lock_.load()) BELL_YIELD();
              tx_lock_.store(true);
              auto t = refresh_token_();
              if (!t.jwt.empty()) {
                setToken(t);           // update token_/endpoint_/token_exp_s_
                if (client_) client_->close(); // force reconnect using fresh token
              }
            }
          }
        }
      }
      BELL_LOG(info, "qws", "disconnected; reconnect in 2s");
      BELL_SLEEP_MS(2000);
    }
  }

private:
  OnOpen on_auth_;
  BytesCallback on_payload_;

  std::unique_ptr<WebSocketClient> client_;
  std::mutex isRunningMutex_;
  std::atomic<bool> isRunning_ = false;
  std::atomic<bool> tx_lock_ = false;
  std::mutex cb_mtx_;
  std::map<uint32_t, Callback> cbs_;

  uint64_t token_exp_s_ = 0;
  uint64_t last_tx_ms_ = 0;
  TokenRefresher refresh_token_;

  uint32_t next_id_ = 0;
  std::string endpoint_;
  std::string token_;
  qconnect_QConnectMessage device = qconnect_QConnectMessage_init_default;
};