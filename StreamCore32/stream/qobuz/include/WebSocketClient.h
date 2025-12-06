#pragma once
#include <string>
#include <vector>
#include <queue>
#include <map>
#include <memory>
#include <mutex>
#include <functional>
#include "TLSSocket.h"
#include "URLParser.h"
#include "BellTask.h"

class WebSocketClient : public bell::Task {
public:
  using OnOpen = std::function<void()>;
  using OnMessage = std::function<void(const std::string&)>;
  using OnClose = std::function<void(int, const std::string&)>;

  using Callback = std::function<void(const std::string& payload)>;

  struct OutMsg {
    uint32_t kind;
    std::vector<uint8_t> payload;
  };
  struct InMsg {
    uint32_t kind;
    std::string payload;
  };

  WebSocketClient() : bell::Task("qobuz_ws_client", 1024 * 5, 5, 1, true) {};
  ~WebSocketClient() { close(); }

  // wss://… URL only (we count on TLS)
  bool connect(const std::string& wss_url,
    const std::string& origin,
    const std::vector<std::string>& subprotocols);
  void setKeepalive(uint32_t pingEveryMs, uint32_t pongTimeoutMs) {
    keepalive_ping_ms_ = pingEveryMs;
    keepalive_pong_timeout_ms_ = pongTimeoutMs;
  }
  void ping();
  uint64_t lastRxMs() const { return last_rx_ms_; }

  bool sendText(const std::string& s);
  bool loopOnce();
  void close(uint16_t code = 1000, const std::string& reason = "");
  bool isOpen() const { return open_; }

  void onOpen(OnOpen f) { on_open_ = std::move(f); }
  void onMessage(OnMessage f) { on_msg_ = std::move(f); }
  void onClose(OnClose f) { on_close_ = std::move(f); }
  std::vector<uint8_t> handleFrame();
  /**
   * @brief Send a WebSocket frame to the server.
   *
   * @param kind The type of frame to send (0x01 for text, 0x02 for binary).
   * @param payload The contents of the frame to send.
   *
   * This function is thread-safe and can be called from any thread.
   * If the socket is not open, this function will return immediately.
   */

  void send(uint32_t kind, const::std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lk(send_mtx_);
    outq_.push({ kind, payload });
  }
  std::vector<uint8_t> pack(uint8_t kind, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    frame.reserve(1 + 10 + payload.size()); // kind + up to 10 bytes varint + data
    frame.push_back(kind);
    writeVarint(payload.size(), frame);
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
  }
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> parse(std::vector<uint8_t>& buf) {
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> out;
    size_t off = 0;

    while (off < buf.size()) {
      if (buf.size() - off < 2) break; // need at least kind + 1 byte of len

      uint8_t kind = buf[off++];

      uint64_t len = 0;
      size_t lenStart = off;
      if (!readVarint(buf, off, len)) { off = lenStart - 1; break; } // wait for more data

      if (off + len > buf.size()) { off = lenStart - 1; break; } // incomplete payload

      out.emplace_back(kind, std::vector<uint8_t>(&buf[off], &buf[off + len])); //std::string((char*)&buf[off], (size_t)len));
      off += (size_t)len;
    }

    // drop consumed bytes
    buf.erase(buf.begin(), buf.begin() + off);
    return out;
  }

private:
  bool handshake(const std::string& url,
    const std::string& origin,
    const std::vector<std::string>& subprotocols);

  bool readFrame(std::string& out);
  bool readFrame(std::vector<uint8_t>& out);
  bool writeFrame(uint8_t opcode, const uint8_t* data, size_t len);
  void runTask() override;
  void pump();
  void pop();

  static inline void writeVarint(uint64_t v, std::vector<uint8_t>& out) {
    while (v >= 0x80) { out.push_back(uint8_t(v) | 0x80); v >>= 7; }
    out.push_back(uint8_t(v));
  }
  static inline bool readVarint(const std::vector<uint8_t>& buf, size_t& off, uint64_t& out) {
    out = 0; int shift = 0;
    for (int i = 0; i < 10 && off < buf.size(); ++i) {
      uint8_t b = buf[off++];
      out |= uint64_t(b & 0x7F) << shift;
      if ((b & 0x80) == 0) return true;
      shift += 7;
    }
    return false; // incomplete or too long
  }
  static std::string genSecKey();

  std::unique_ptr<bell::TLSSocket> tls_;
  bool open_ = false;

  OnOpen on_open_;
  OnMessage on_msg_;
  OnClose on_close_;

  std::mutex isRunningMutex_;
  std::atomic<bool> isRunning_;
  // keepalive config/state
  uint32_t keepalive_ping_ms_ = 30000;       // send a Ping every 30s
  uint32_t keepalive_pong_timeout_ms_ = 10000; // fail if >10s with no Pong
  std::atomic<uint64_t> last_rx_ms_{ 0 };
  std::atomic<uint64_t> last_tx_ms_{ 0 };
  std::atomic<bool> awaiting_pong_{ false };
  // outgoing
  std::mutex send_mtx_;
  std::queue<OutMsg> outq_;

  // incoming raw frames -> parsed -> callbacks
  std::mutex in_mtx_;
  std::queue<std::vector<uint8_t>> inq_raw_;

  std::mutex cb_mtx_;
  std::map<uint32_t, Callback> cbs_;
};
