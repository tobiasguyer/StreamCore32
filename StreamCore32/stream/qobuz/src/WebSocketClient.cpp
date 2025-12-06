#include "WebSocketClient.h"
#include <algorithm>
#include <cstring>
#include "EspRandomEngine.h" // esp_randomn
#include "TimeSync.h"

#include "Logger.h"

static std::string b64(const uint8_t* d, size_t n) {
  static const char B[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o; o.reserve(((n + 2) / 3) * 4);
  size_t i = 0;
  while (i + 2 < n) {
    uint32_t v = (uint32_t(d[i]) << 16) | (uint32_t(d[i + 1]) << 8) | d[i + 2];
    o.push_back(B[(v >> 18) & 63]); o.push_back(B[(v >> 12) & 63]);
    o.push_back(B[(v >> 6) & 63]);  o.push_back(B[v & 63]); i += 3;
  }
  if (i + 1 == n) {
    uint32_t v = (uint32_t(d[i]) << 16);
    o.push_back(B[(v >> 18) & 63]); o.push_back(B[(v >> 12) & 63]); o.push_back('='); o.push_back('=');
  }
  else if (i + 2 == n) {
    uint32_t v = (uint32_t(d[i]) << 16) | (uint32_t(d[i + 1]) << 8);
    o.push_back(B[(v >> 18) & 63]); o.push_back(B[(v >> 12) & 63]); o.push_back(B[(v >> 6) & 63]); o.push_back('=');
  }
  return o;
}
std::string WebSocketClient::genSecKey() {
  uint8_t r[16];
  for (int i = 0;i < 16;i += 4) { uint32_t w = streamcore::esp_random_engine()(); std::memcpy(r + i, &w, std::min(4, 16 - i)); }
  return b64(r, 16);
}

bool WebSocketClient::connect(const std::string& url,
  const std::string& origin,
  const std::vector<std::string>& subs) {
  // 1st attempt: with provided subprotocols ({"qws"} by default)
  if (handshake(url, origin, subs)) return true;

  // 2nd attempt: no subprotocols at all
  SC32_LOG(error, "handshake retry without subprotocols");
  if (handshake(url, origin, {})) return true;

  return false;
}

bool WebSocketClient::handshake(const std::string& url,
  const std::string& origin,
  const std::vector<std::string>& subs)
{
  bell::URLParser u = bell::URLParser::parse(url);
  if (u.schema != "wss") { SC32_LOG(error, "non-wss schema: %s", u.schema.c_str()); return false; }
  const std::string host = u.host;
  const int port = (u.port > 0) ? u.port : 443;
  std::string path = u.path.empty() ? "/" : u.path;

  tls_.reset(new bell::TLSSocket());
  tls_->open(host, (uint16_t)port);
  if (!tls_->isOpen()) { SC32_LOG(error, "TLS open failed"); return false; }

  const std::string seckey = genSecKey();
  std::string req;
  req.reserve(768);
  req += "GET " + path + " HTTP/1.1\r\n";
  req += "Host: " + host + "\r\n";
  req += "Upgrade: websocket\r\n";
  req += "Connection: Upgrade\r\n";
  req += "Sec-WebSocket-Version: 13\r\n";
  req += "Sec-WebSocket-Key: " + seckey + "\r\n";
  req += "Origin: " + origin + "\r\n";

  // add a couple of browser-ish headers (harmless, often expected by edge infra)
  req += "User-Agent: Mozilla/5.0\r\n";
  req += "Pragma: no-cache\r\n";
  req += "Cache-Control: no-cache\r\n";
  // Many WS servers advertise this; not required, but some balancers like it:
  req += "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n";

  if (!subs.empty()) {
    req += "Sec-WebSocket-Protocol: ";
    for (size_t i = 0;i < subs.size();++i) { if (i) req += ", "; req += subs[i]; }
    req += "\r\n";
  }
  req += "\r\n";

  if (tls_->write(reinterpret_cast<const uint8_t*>(req.data()), req.size()) <= 0) {
    SC32_LOG(error, "write handshake failed");
    return false;
  }

  // Read headers until CRLFCRLF; log what we got if it’s not a 101
  // Read headers until CRLFCRLF; do not gate on poll() – pull data actively
  std::string hdr; hdr.reserve(2048);
  uint8_t ch = 0;
  uint32_t waited_ms = 0;

  while (waited_ms < 6000) {
    // try to read 1 byte; TLSSocket::read() already internally handles WANT_READ/WRITE
    ssize_t r = tls_->read(&ch, 1);
    if (r == 1) {
      hdr.push_back((char)ch);
      if (hdr.size() >= 4 && hdr.compare(hdr.size() - 4, 4, "\r\n\r\n") == 0) break;
      if (hdr.size() > 16384) { SC32_LOG(error, "header too large"); return false; }
      // don’t count time if we’re receiving bytes
      continue;
    }
    if (r == 0) { SC32_LOG(error, "server closed during handshake"); return false; }
    // r < 0 -> TLSSocket already logged; brief backoff and keep waiting a bit
    BELL_SLEEP_MS(10);
    waited_ms += 10;
  }

  if (hdr.find(" 101 ") == std::string::npos) {
    std::string show = hdr.substr(0, std::min<size_t>(hdr.size(), 512));
    for (auto& c : show) if ((unsigned)c < 32 && c != '\r' && c != '\n') c = '.';
    SC32_LOG(error, "WS handshake non-101. First header bytes:\n---\n%s\n---", show.c_str());
    return false;
  }


  // Optional: check Upgrade and Connection headers if you want to be strict
  open_ = true;

  // seed keepalive clocks
  const uint64_t now = timesync::now_ms();
  last_rx_ms_ = now;
  last_tx_ms_ = now;
  awaiting_pong_ = false;

  if (on_open_) on_open_();
  return true;
}

void WebSocketClient::ping() { writeFrame(0x9, nullptr, 0); awaiting_pong_ = true; last_tx_ms_ = timesync::now_ms(); }
// Masked text frame (client -> server)
bool WebSocketClient::writeFrame(uint8_t opcode, const uint8_t* data, size_t len) {
  if (!open_) return false;
  uint8_t mask[4]; for (int i = 0;i < 4;++i) mask[i] = uint8_t(esp_random() & 0xFF);

  std::vector<uint8_t> buf; buf.reserve(2 + 8 + 4 + len);
  buf.push_back(0x80 | (opcode & 0x0F));
  if (len < 126) { buf.push_back(0x80 | uint8_t(len)); }
  else if (len <= 0xFFFF) { buf.push_back(0x80 | 126); buf.push_back((len >> 8) & 0xFF); buf.push_back(len & 0xFF); }
  else { buf.push_back(0x80 | 127); for (int i = 7;i >= 0;--i) buf.push_back(uint8_t((uint64_t(len) >> (i * 8)) & 0xFF)); }
  buf.insert(buf.end(), mask, mask + 4);
  size_t off = buf.size();
  buf.resize(off + len);
  for (size_t i = 0;i < len;++i) buf[off + i] = data[i] ^ mask[i & 3];

  bool ok = tls_->write(buf.data(), buf.size()) == (ssize_t)buf.size();
  if (ok) last_tx_ms_ = timesync::now_ms();
  return ok;
}

bool WebSocketClient::sendText(const std::string& s) {
  return writeFrame(0x1, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// Read one frame (handles text, ping/pong, close). Returns true only for text.
bool WebSocketClient::readFrame(std::string& out) {
  std::vector<uint8_t> v;
  if (!readFrame(v)) return false;
  out.assign(v.begin(), v.end());
  return true;
}
bool WebSocketClient::readFrame(std::vector<uint8_t>& out) {
  uint8_t h[2];// short poll to avoid blocking on ssl_read
  if (tls_->poll_readable(0) <= 0) {
    return false;
  }
  if (tls_->read(h, 2) != 2) return false;

  uint8_t opcode = h[0] & 0x0F;
  bool masked = (h[1] >> 7) & 1;
  uint64_t len = (h[1] & 0x7F);
  if (len == 126) { uint8_t e[2]; if (tls_->read(e, 2) != 2) return false; len = (e[0] << 8) | e[1]; }
  else if (len == 127) { uint8_t e[8]; if (tls_->read(e, 8) != 8) return false; len = 0; for (int i = 0;i < 8;++i) len = (len << 8) | e[i]; }
  uint8_t mask[4] = { 0,0,0,0 };
  if (masked) {
    if (tls_->read(mask, 4) != 4) {
      SC32_LOG(error, "failed to read mask"); return false;
    }
  }

  std::string payload; payload.resize((size_t)len);
  size_t got = 0;
  while (got < len) {
    ssize_t r = tls_->read(reinterpret_cast<uint8_t*>(&payload[got]), (size_t)(len - got));
    if (r <= 0) { BELL_SLEEP_MS(5); SC32_LOG(error, "yield to read frame, got=%d", got); continue; }
    got += (size_t)r;
  }

  if (masked) { for (size_t i = 0;i < len;++i) payload[i] ^= mask[i & 3]; }

  last_rx_ms_ = timesync::now_ms();

  if (opcode == 0xA) {           // PONG
    awaiting_pong_ = false;
    return false;
  }
  if (opcode == 0x1 || opcode == 0x2) { out = { payload.begin(), payload.end() }; return true; }   // text / binary
  if (opcode == 0x9) {
    writeFrame(0xA, reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    return false;
  }
  else SC32_LOG(error, "unknown opcode %d", opcode);
  if (opcode == 0x8) { open_ = false; if (on_close_) on_close_(1000, "server-close"); return false; }
  return false;
}

void WebSocketClient::pump() {
  std::queue<OutMsg> local;
  {
    std::lock_guard<std::mutex> lk(send_mtx_);
    std::swap(local, outq_);
  }
  if (!open_) return;
  while (!local.empty()) {
    const auto& m = local.front();
    writeFrame(m.kind, m.payload.data(), m.payload.size());
    local.pop();
  }
}


std::vector<uint8_t> WebSocketClient::handleFrame() {

  std::vector<uint8_t> frame = {};
  {
    std::lock_guard<std::mutex> lk(in_mtx_);
    if (!inq_raw_.empty()) {
      frame = std::move(inq_raw_.front());
      inq_raw_.pop();
    }
  }
  return frame;
}


bool WebSocketClient::loopOnce() {
  if (!open_) return false;
  this->pump();
  std::vector<uint8_t> msg;
  if (readFrame(msg)) {
    std::lock_guard<std::mutex> lk(in_mtx_);
    inq_raw_.push(msg);
    return true;
  }
  return false;
}
void WebSocketClient::runTask() {
  isRunning_.store(true);
  std::lock_guard<std::mutex> lk(isRunningMutex_);
  while (isRunning_.load()) {
    (void)loopOnce();  // reads/pumps if available

    // --- keepalive watchdog ---
    const uint64_t now = timesync::now_ms();
    // Send periodic Ping if quiet for a while
    if (open_ && (now - last_tx_ms_) >= keepalive_ping_ms_ && !awaiting_pong_) {
      ping();
    }
    // If we were awaiting a Pong and rx stayed silent too long -> timeout
    if (open_ && awaiting_pong_ && (now - last_rx_ms_) >= keepalive_pong_timeout_ms_) {
      SC32_LOG(error, "timeout: no PONG in %ums (last_rx=%llu, now=%llu)",
        keepalive_pong_timeout_ms_, (unsigned long long)last_rx_ms_, (unsigned long long)now);
      isRunning_.store(false);
      open_ = false;
      if (tls_) tls_->close();
      if (on_close_) on_close_(1001, "ping-timeout");   // triggers on_close_ callback
      continue;  // let outer logic decide reconnection
    }

    BELL_SLEEP_MS(100);
  }
}
void WebSocketClient::close(uint16_t, const std::string&) {
  if (!open_) return;
  isRunning_.store(false);
  std::lock_guard<std::mutex> lk(isRunningMutex_);
  open_ = false;
  if (tls_) tls_->close();
  if (on_close_) on_close_(1000, "client-close");
}
