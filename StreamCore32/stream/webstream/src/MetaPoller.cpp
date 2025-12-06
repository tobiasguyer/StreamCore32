#include "MetaPoller.h"
#include "UrlOrigin.h"
#include <set>
void MetaPoller::runTask() {
  isRunning_.store(true);
  wantStop_.store(false);
  using streamcore::helpers::genOriginVariants;
  constexpr size_t kMaxUrlsPerCycle = 12;
  constexpr size_t kMaxAcceptBody = 12 * 1024;

  while (isRunning_.load()) {
    if (wantStop_.load()) { isRunning_.store(false); break; }
    if (!active_.load()) {
      StreamBase::sleepMs(200);
      continue;
    }

    Spec spec;
    std::string origin;
    std::string station;
    {
      std::lock_guard<std::mutex> lk(mu_);
      spec = spec_;
      origin = origin_;
      station = station_;
    }

    std::vector<std::string> urls;
    std::set<std::string> seen;
    auto push = [&](const std::string& u) { if (!u.empty() && !seen.count(u)) {
      urls.push_back(u);
      seen.insert(u);
    } };

    if (!lockedUrl_.empty()) push(lockedUrl_);
    else {
      if (!spec.url.empty()) {
        if (StreamBase::startsWith(spec.url, "http://") || StreamBase::startsWith(spec.url, "https://")) push(spec.url);
        else push(origin + (spec.url[0] == '/' ? "" : "/") + spec.url);
      }
      for (const auto& o : genOriginVariants(origin)) {
        if (spec.kind == Kind::Auto || spec.kind == Kind::IcecastJSON) push(o + "/status-json.xsl");
        if (spec.kind == Kind::Auto || spec.kind == Kind::ShoutcastJSON) push(o + "/stats?json=1");
        if (spec.kind == Kind::Auto || spec.kind == Kind::Shoutcast7) push(o + "/7.html");
        // generic site JSON now-playing
        push(o + "/tracklist/currentlyplaying.json");
      }
    }

    std::string title;
    size_t tried = 0;
    for (auto& u : urls) {
      if (++tried > kMaxUrlsPerCycle) break;
      auto resp = httpGetSimple(u);
      if (!resp) {
        StreamBase::sleepMs(50);
        continue;
      }
      int code = statusFromHeaders(*resp);
      if (code < 200 || code >= 300) {
        if (u == lockedUrl_) {
          if (++lockedFailures_ >= 3) {
            lockedUrl_.clear();
            lockedFailures_ = 0;
          }
        } StreamBase::sleepMs(25);
        continue;
      }
      auto ctL = StreamBase::toLower(StreamBase::svToString(resp->header("content-type")));
      size_t clen = sizeFromHeader(resp->header("content-length"));
      if (clen > kMaxAcceptBody) {
        StreamBase::sleepMs(25);
        continue;
      }
      auto ul = StreamBase::toLower(u);
      bool expectJson = (StreamBase::endsWith(ul, "status-json.xsl") || ul.find("stats?json") != std::string::npos || StreamBase::endsWith(ul, ".json"));
      bool expectText = (StreamBase::endsWith(ul, "/7.html") || ul.find("/7.html?") != std::string::npos);
      if (expectJson && (!ctL.empty() && ctL.find("json") == std::string::npos)) {
        StreamBase::sleepMs(10);
        continue;
      }
      if (expectText && (!ctL.empty() && ctL.find("text") == std::string::npos)) {
        StreamBase::sleepMs(10);
        continue;
      }
      auto body_sv = resp->body();
      if (body_sv.size() > kMaxAcceptBody) {
        StreamBase::sleepMs(10);
        continue;
      }
      std::string body(body_sv.data(), body_sv.size());

      if (StreamBase::endsWith(ul, "status-json.xsl")) {
        nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        if (!j.is_discarded() && j.contains("icestats")) {
          const auto& ic = j["icestats"];
          if (ic.contains("source")) {
            const auto& src = ic["source"];
            if (src.is_array()) {
              for (const auto& s : src) {
                if (!title.empty()) break;
                title = pickIcecastTitle(s);
              }
            }
            else if (src.is_object()) {
              title = pickIcecastTitle(src);
            }
          }
        }
      }
      else if (ul.find("stats?json") != std::string::npos) {
        nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        if (!j.is_discarded()) {
          title = j.value("songtitle", std::string());
          if (title.empty()) title = j.value("title", std::string());
        }
      }
      else if (StreamBase::endsWith(ul, "/7.html") || ul.find("/7.html?") != std::string::npos) {
        title = parseShoutcast7(body);
      }
      else if (StreamBase::endsWith(ul, ".json")) {
        nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        if (!j.is_discarded()) {
          std::string a = j.value("artist", std::string());
          std::string t = j.value("track", std::string());
          if (!a.empty() && !t.empty()) title = a + " - " + t;
          else if (!t.empty()) title = t;
          if (title.empty()) title = j.value("track", std::string());
        }
      }

      if (!title.empty()) {
        lockedUrl_ = u;
        lockedFailures_ = 0;
        break;
      }
      StreamBase::sleepMs(25);
    }
    if (title.empty() && lockedUrl_.empty()) {
      active_.store(false);
      lastTitle_.clear();
      continue;
    }
    if (!title.empty() && title != lastTitle_) {
      lastTitle_ = title;
      if (emit_) emit_(station, title);
    }

    uint32_t base = spec.intervalMs > 500 ? spec.intervalMs : 1000;
    uint32_t jitter = (xTaskGetTickCount() % 250);
    uint32_t remain = base + jitter;
    while (remain && active_.load()) {
      StreamBase::sleepMs(100);
      remain = (remain > 100) ? (remain - 100) : 0;
    }
    if (wantStop_.load()) isRunning_.store(false);
  }
}

std::unique_ptr<bell::HTTPClient::Response> MetaPoller::httpGetSimple(const std::string& url) {
  bell::HTTPClient::Headers h = { {"User-Agent", "StreamCore32/Radio (ESP-IDF/Bell)"}, {"Accept","application/json, text/plain;q=0.9, */*;q=0.5"} };
  return bell::HTTPClient::get(url, h, 12);
}
int MetaPoller::statusFromHeaders(bell::HTTPClient::Response& r) {
  auto sv = r.header(":status");
  if (!sv.empty()) return StreamBase::toInt(sv);
  sv = r.header("status");
  if (!sv.empty()) return StreamBase::toInt(sv);
  sv = r.header("Status");
  if (!sv.empty()) return StreamBase::toInt(sv);
  return 200;
}
size_t MetaPoller::sizeFromHeader(std::string_view sv) {
  size_t v = 0;
  for (char c : sv) {
    if (c < '0' || c>'9') break;
    v = v * 10 + (size_t)(c - '0');
    if (v > (1u << 30)) break;
  } return v;
}
std::string MetaPoller::pickIcecastTitle(const json& s) {
  std::string title = s.value("title", std::string());
  std::string artist = s.value("artist", std::string());
  if (!artist.empty() && !title.empty()) return artist + " - " + title;
  return title;
}
std::string MetaPoller::parseShoutcast7(const std::string& body) {
  size_t start = 0;
  int field = 0;
  for (size_t i = 0;i < body.size();++i) {
    if (body[i] == ',') {
      ++field;
      if (field == 4) {
        start = i + 1;
        break;
      }
    }
  } if (start == 0) return "";
  size_t end = body.find(',', start);
  if (end == std::string::npos) end = body.size();
  std::string t = body.substr(start, end - start);
  StreamBase::trim(t);
  auto lt = t.find('<');
  if (lt != std::string::npos) t.erase(lt);
  return t;
}

