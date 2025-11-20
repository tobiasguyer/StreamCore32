// ============================
// include/streamcore/WebStream.h
// ============================
#pragma once
#include <memory>
#include <string>
#include <mutex>
#include <vector>
#include <optional>
#include <algorithm>
#include <nlohmann/json.hpp>

#include "BellTask.h"
#include "AudioControl.h"
#include "HTTPClient.h"
#include "Logger.h"
#include "MetaPoller.h"   // your existing poller helper

class WebStream : public StreamBase {
public:
  using MetaCb  = std::function<void(const std::string&, const std::string&)>;
  using ErrorCb = std::function<void(const std::string&)>;
  using StateCb = std::function<void(bool)>;

  struct MetaSpec {
    MetaPoller::Kind kind      = MetaPoller::Kind::Auto;
    std::string      url;                // optional explicit endpoint (abs or relative)
    uint32_t         intervalMs = 5000;  // polling cadence
    bool             enabled    = true;
    bool             fallbackOnEmptyICY = true; // keep poller alive if ICY is empty
    bool             autoDisarmOnICY    = true; // disarm poller once non-empty ICY seen
  };

  WebStream(std::shared_ptr<AudioControl> audio)
  : StreamBase("WebStream", audio, 1024*32, 1, 1, false)
  {
    // metadata poller (idles until armed)
    poller_ = std::make_shared<MetaPoller>(
      [this](const std::string& s, const std::string& t){ if(onMeta_) onMeta_(s,t); },
      [this](const std::string& m){ if(onError_) onError_(m); }
    );
    poller_->startTask();
  }

  ~WebStream() override {
    if (poller_) {
      poller_->stopTask();
      while (poller_->isRunning()) vTaskDelay(pdMS_TO_TICKS(25));
    }
    if(isRunning_.load()) stop();
    
  }

  // ---- callbacks ----
  void onMetadata(MetaCb cb) { onMeta_  = std::move(cb); }
  void onError(ErrorCb cb)   { onError_ = std::move(cb); }
  void onState(StateCb cb)   { onState_ = std::move(cb); }

  // ---- config ----
  void setMetaSpec(const MetaSpec& s) { std::lock_guard<std::mutex> lk(mu_); metaSpec_ = s; }

  // ---- control ----
  // Can be called while playing — causes a seamless restart to new URI
  void play(const std::string& uri, const std::string& displayName = std::string()) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      targetUri_   = uri;
      displayName_ = displayName;
    }
    wantStop_.store(false);
    wantRestart_.store(true);
    if (!isRunning_.load()) startTask();
  }

  void stop() {
    wantStop_.store(true);
    wantRestart_.store(false);
    SPOTIFY_LOG(info, "Stopping...");
    if (poller_) poller_->disarm();
  }

protected:
  void runTask() override {
    isRunning_.store(true);

    while (isRunning_.load()) {
      if (!wantRestart_.load()) { vTaskDelay(pdMS_TO_TICKS(25)); continue; }

      std::string name;
      {
        std::lock_guard<std::mutex> lk(mu_);
        resolvedUri_  = targetUri_;
        name = displayName_;
      }
      wantRestart_.store(false);
      if (resolvedUri_.empty()) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

      // Resolve possible playlists
      auto resolved = resolveIfPlaylist(resolvedUri_);
      if (!resolved) {
        reportError("resolve failed");
        vTaskDelay(pdMS_TO_TICKS(1000));
        wantRestart_.store(true);
        continue;
      }
      wantRestart_.store(true);
      // bump track id so the sink treats it as new
      const uint32_t tid = audio_->makeUniqueTrackId();
      if (onState_) onState_(true);
      auto resp = open(*resolved, name, tid);
      if(resp == nullptr) continue;
      const size_t CHUNK=1024; uint8_t buf[CHUNK]; int ret = 0;
      while(!wantStop_.load()){
        ret = read(resp->stream(), buf, 1024, tid);
        if(ret <= 0) break;
        uint16_t written = 0;
        while(written < ret && !wantStop_.load()){
          uint16_t ret_ = feed_->feedData(buf + written, ret - written, tid, false);
          if(ret_ == 0) BELL_SLEEP_MS(10);
          written += ret_;
        }
      }

      //const bool cleanEnd = streamOnce(*resolvedUri_, name, tid);
      if (onState_) onState_(false);

      if (wantStop_.load() && !wantRestart_.load()) {
        if (feed_) {
          feed_->feedCommand(AudioControl::FLUSH, 0);
          feed_->feedCommand(AudioControl::DISC,  0);
        }
        if (poller_) poller_->disarm();
        wantStop_.store(false);
        isRunning_.store(false);
        SPOTIFY_LOG(info, "Stopped.");
        break;
      } else {
        if (feed_) feed_->feedCommand(AudioControl::SKIP, 0);
        if (poller_) poller_->disarm();
        vTaskDelay(pdMS_TO_TICKS(reconnectDelayMs_));
        // reconnect if unexpected end or user queued another play
        wantRestart_.store(true);
      }
    }
  }

private:
  std::unique_ptr<bell::HTTPClient::Response> open(const std::string& url, const std::string& station, uint32_t trackId) {

    bell::HTTPClient::Headers hdrs = { {"Icy-MetaData","1"}, {"User-Agent", ua_} };
    auto resp = bell::HTTPClient::get(url, hdrs, 32);
    if(!resp){ reportError("HTTP connect failed"); return nullptr; }

    H.metaInt = toInt(resp->header("icy-metaint"));
    H.contentType = svToString(resp->header("content-type"));
    H.stationName = svToString(resp->header("icy-name"));
    if(H.stationName.empty()) H.stationName = station;
    SPOTIFY_LOG(info, "headers: %s %d %s", H.contentType.c_str(), H.metaInt, H.stationName.c_str());

    hadNonEmptyICY_ = false;
    if (poller_ && metaSpec_.enabled && metaSpec_.kind != MetaPoller::Kind::Disabled){
      MetaPoller::Spec ps; ps.kind=(MetaPoller::Kind)metaSpec_.kind; ps.url=metaSpec_.url; ps.intervalMs=metaSpec_.intervalMs; ps.enabled=metaSpec_.enabled;
      if(H.metaInt<=0) poller_->arm(originFromUrl(url), H.stationName, ps);
      else if(metaSpec_.fallbackOnEmptyICY && H.stationName.empty()) poller_->arm(originFromUrl(url), H.stationName, ps);
      else poller_->disarm();
    }
    bytesUntilMeta = (H.metaInt>0)? H.metaInt : std::numeric_limits<int>::max();
    return resp;
  }
  int read(bell::SocketStream &stream, uint8_t *buffer, size_t chunk_size, size_t trackId) {

      size_t want = (size_t)std::min<int>(bytesUntilMeta, (int)chunk_size);
      stream.read(reinterpret_cast<char*>(buffer), want); auto got=stream.gcount(); if(got<=0) return got;
      bytesUntilMeta -= (int)got;
      if(bytesUntilMeta==0){  
        int L=readByte(stream);
        if(L<0) return L;
        int metaLen=L*16;
        if(metaLen>0) 
        { std::string meta;
          meta.resize((size_t)metaLen);
          readExact(stream, reinterpret_cast<uint8_t*>(&meta[0]), metaLen);
          parseAndEmitIcy(meta, H.stationName);
        } else {
          MetaPoller::Spec ps; ps.kind=MetaPoller::Kind::Auto; ps.url=metaSpec_.url;
          ps.intervalMs=metaSpec_.intervalMs; ps.enabled=metaSpec_.enabled;
          poller_->arm(originFromUrl(resolvedUri_), H.stationName, ps);
        }
        bytesUntilMeta = (H.metaInt>0)? H.metaInt : std::numeric_limits<int>::max();
      }
      return got;

  }
  // ---------- playlist helpers ----------
  static bool hasPlaylistExt(const std::string& u){ auto L=toLower(u); return endsWith(L,".m3u")||endsWith(L,".m3u8")||endsWith(L,".pls"); }

  std::optional<std::string> resolveIfPlaylist(const std::string& url){
    if (hasPlaylistExt(url)) return fetchPlaylist(url);
    bell::HTTPClient::Headers hdrs = { {"Icy-MetaData","1"}, {"User-Agent", ua_} };
    auto resp = bell::HTTPClient::get(url, hdrs, 32); if(!resp) return std::nullopt;
    auto ctype = svToString(resp->header("content-type"));
    if (startsWith(toLower(ctype), "audio/")) return url;
    if (isPlaylistContentType(ctype)) {
      auto body_sv = resp->body(); std::string body(body_sv.data(), body_sv.size());
      return parsePlaylistBody(body);
    }
    auto body_sv = resp->body(); if (!body_sv.empty()){
      std::string body(body_sv.data(), body_sv.size()); if(auto u2 = parsePlaylistBody(body)) return u2; }
    return url;
  }

  std::optional<std::string> fetchPlaylist(const std::string& url){
    auto resp = bell::HTTPClient::get(url, { {"User-Agent", ua_} }, 32); if(!resp) return std::nullopt;
    auto body_sv = resp->body(); std::string body(body_sv.data(), body_sv.size());
    return parsePlaylistBody(body);
  }

  static bool isPlaylistContentType(const std::string& ct){ auto L=toLower(ct); return startsWith(L,"audio/x-mpegurl")||startsWith(L,"application/vnd.apple.mpegurl")||startsWith(L,"application/x-mpegURL")||startsWith(L,"application/pls")||startsWith(L,"audio/x-scpls")||startsWith(L,"text/"); }

  static std::optional<std::string> parsePlaylistBody(const std::string& body){ size_t pos=0; bool first=true; while(pos<body.size()){ size_t end=body.find_first_of("\r\n", pos); size_t len=(end==std::string::npos)?(body.size()-pos):(end-pos); std::string line=body.substr(pos,len); if(end==std::string::npos) pos=body.size(); else pos=(body[end]=='\r' && end+1<body.size() && body[end+1]=='\n')? end+2 : end+1; if(first && line.size()>=3 && (unsigned char)line[0]==0xEF && (unsigned char)line[1]==0xBB && (unsigned char)line[2]==0xBF) line.erase(0,3); first=false; trim(line); if(line.empty()) continue; if(line[0]=='#'||line[0]==';'||line[0]=='[') continue; auto eq=line.find('='); std::string cand=(eq!=std::string::npos)?line.substr(eq+1):line; trim(cand); if(startsWith(cand,"http://")||startsWith(cand,"https://")) return cand; } return std::nullopt; }

  // ---------- streaming ----------

  bool streamOnce(const std::string& url, const std::string& station, uint32_t trackId){
    bell::HTTPClient::Headers hdrs = { {"Icy-MetaData","1"}, {"User-Agent", ua_} };
    auto resp = bell::HTTPClient::get(url, hdrs, 32);
    if(!resp){ reportError("HTTP connect failed"); return false; }

    IcyHeaders H; 
    H.metaInt = toInt(resp->header("icy-metaint"));
    H.contentType = svToString(resp->header("content-type"));
    H.stationName = svToString(resp->header("icy-name"));
    if(H.stationName.empty()) H.stationName = station;
    SPOTIFY_LOG(info, "headers: %s %d %s", H.contentType.c_str(), H.metaInt, H.stationName.c_str());

    hadNonEmptyICY_ = false;
    if (poller_ && metaSpec_.enabled && metaSpec_.kind != MetaPoller::Kind::Disabled){
      MetaPoller::Spec ps; ps.kind=(MetaPoller::Kind)metaSpec_.kind; ps.url=metaSpec_.url; ps.intervalMs=metaSpec_.intervalMs; ps.enabled=metaSpec_.enabled;
      if(H.metaInt<=0) poller_->arm(originFromUrl(url), H.stationName, ps);
      else if(metaSpec_.fallbackOnEmptyICY && H.stationName.empty()) poller_->arm(originFromUrl(url), H.stationName, ps);
      else poller_->disarm();
    }

    auto& is = resp->stream();
    const size_t CHUNK=1024; std::vector<uint8_t> buf(CHUNK);
    int bytesUntilMeta = (H.metaInt>0)? H.metaInt : std::numeric_limits<int>::max();
    isRunning_.store(true);
    while(isRunning_.load()){
      size_t want = (size_t)std::min<int>(bytesUntilMeta, (int)CHUNK);
      is.read(reinterpret_cast<char*>(buf.data()), want); auto got=is.gcount(); if(got<=0) break;
      size_t fed = feed_->feedData(buf.data(), (size_t)got, trackId, false); if(fed==0) vTaskDelay(pdMS_TO_TICKS(1));
      bytesUntilMeta -= (int)got;
      if(bytesUntilMeta==0){
        int L=readByte(is);
        if(L<0) break;
        int metaLen=L*16;
        if(metaLen>0) 
        { std::string meta;
          meta.resize((size_t)metaLen);
          readExact(is, reinterpret_cast<uint8_t*>(&meta[0]), metaLen);
          parseAndEmitIcy(meta, H.stationName);
        } else {
          MetaPoller::Spec ps; ps.kind=MetaPoller::Kind::Auto; ps.url=metaSpec_.url;
          ps.intervalMs=metaSpec_.intervalMs; ps.enabled=metaSpec_.enabled;
          poller_->arm(originFromUrl(url), H.stationName, ps);
        }
        bytesUntilMeta = (H.metaInt>0)? H.metaInt : std::numeric_limits<int>::max();
      }
      vTaskDelay(pdMS_TO_TICKS(1));
    }

    return true; // true if asked to stop; false = unexpected end
  }

  // ---------- ICY helpers ----------
  static std::string parseStreamTitle(const std::string& meta){ std::string m=meta; m.erase(std::remove(m.begin(), m.end(), '\0'), m.end()); std::string ml=toLower(m); const std::string key="streamtitle="; size_t p=ml.find(key); if(p==std::string::npos) return ""; size_t v=p+key.size(); if(v>=m.size()) return ""; char q=m[v]; size_t s,e; if(q=='\''||q=='"'){ s=v+1; e=m.find(q,s);} else { s=v; e=m.find(';',s);} if(e==std::string::npos) e=m.size(); std::string val=m.substr(s,e-s); trim(val); return val; }
  void parseAndEmitIcy(const std::string& raw, const std::string& station){
    auto title=parseStreamTitle(raw);
    if(!title.empty()){
      hadNonEmptyICY_=true;
      if(onMeta_) onMeta_(station,title);
      if(poller_ && metaSpec_.autoDisarmOnICY) poller_->disarm();
    }
  }

  // ---------- tiny utils ----------
  static std::string toLower(std::string s){ for(char& c: s) c=(char)std::tolower((unsigned char)c); return s; }
  static bool startsWith(const std::string& s, const char* pfx){ return s.rfind(pfx,0)==0; }
  static bool endsWith(const std::string& s, const char* sfx){ size_t n=strlen(sfx); return s.size()>=n && std::equal(s.end()-n,s.end(),sfx); }
  static void ltrim(std::string& s){ s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch){return !std::isspace(ch);})); }
  static void rtrim(std::string& s){ s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch){return !std::isspace(ch);}).base(), s.end()); }
  static void trim(std::string& s){ ltrim(s); rtrim(s); }
  static int  toInt(std::string_view sv){ int v=0; for(char c:sv) if(std::isdigit((unsigned char)c)) v=v*10+(c-'0'); return v; }
  static std::string svToString(std::string_view sv){ return std::string(sv.data(), sv.size()); }
  static int readByte(std::iostream& is){ char c; is.read(&c,1); if(is.gcount()<=0) return -1; return (unsigned char)c; }
  static void readExact(std::iostream& is, uint8_t* dst, int n){ int got=0; while(got<n){ is.read(reinterpret_cast<char*>(dst+got), n-got); auto g=is.gcount(); if(g<=0) break; got += (int)g; } }
  static std::string originFromUrl(const std::string& url)
  { auto p=url.find("://"); if(p==std::string::npos) return url;
    auto start=p+3; auto slash=url.find('/', start);
    if(slash==std::string::npos) return url.substr(0, url.size());
    return url.substr(0, slash);
  }

private:
  std::shared_ptr<MetaPoller>                  poller_;

  // state
  std::atomic<bool> isRunning_{false};
  std::atomic<bool> wantStop_{false};
  std::atomic<bool> wantRestart_{false};

  std::mutex   mu_;
  std::string  targetUri_;
  std::string  resolvedUri_;
  std::string  displayName_;
  uint32_t     trackId_ = 0;

  MetaSpec     metaSpec_{};
  struct IcyHeaders { int metaInt=0; std::string contentType; std::string stationName; };
  IcyHeaders   H{}; 
  bool         hadNonEmptyICY_ = false;
  int          bytesUntilMeta = std::numeric_limits<int>::max();
  // callbacks
  MetaCb  onMeta_;
  ErrorCb onError_;
  StateCb onState_;

  const uint32_t reconnectDelayMs_ = 1500;
  static constexpr const char* ua_ = "StreamCore32/WebStream (ESP32)";
};
