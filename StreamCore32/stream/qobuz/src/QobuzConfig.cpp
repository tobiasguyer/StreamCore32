#include "QobuzConfig.h"

#include <cstring>
#include <regex>
#include <sstream>
#include <stdexcept>

#include "HTTPClient.h"
#include "Logger.h"
#include "TLSSocket.h"
#include "URLParser.h"
#include "X509Bundle.h"

#include "SocketStream.h"
// --- helpers: URL resolution, HTTP GET over Bell ---
// Qobuz bootstrap endpoints (mirrors the helper’s approach)

namespace {

static std::string buildPath(const bell::URLParser& u) {
  return u.path.empty() ? "/" : u.path;
}

static void writeHttp11Get(bell::SocketStream& s, const bell::URLParser& u) {
  std::string path = buildPath(u);
  std::string req;
  req.reserve(512);
  req += "GET " + path + " HTTP/1.1\r\n";
  req += "Host: " + u.host + "\r\n";
  req +=
      "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
      "Chrome/124 Safari/537.36\r\n";
  req += "Accept: application/json,*/*;q=0.8\r\n";
  req += "Accept-Language: en-US,en;q=0.9\r\n";
  req += "Accept-Encoding: identity\r\n";         // avoid gzip on MCU
  req += "Connection: close\r\n";                 // read-to-EOF
  req += "Referer: https://play.qobuz.com/\r\n";  // helps sometimes
  req += "\r\n";
  s << req;
  s.flush();
}

// base64url decode (URL_SAFE, no padding required)
static inline uint8_t b64lut(char c) {
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '-' || c == '_')
    return (c == '-') ? 62 : 63;
  return 0xFF;
}

bool base64url_decode_inplace(std::string& s) {
  // add padding to multiple of 4
  size_t mod = s.size() % 4;
  if (mod)
    s.append(4 - mod, '=');

  std::string out;
  out.reserve((s.size() * 3) / 4);
  uint32_t val = 0;
  int valb = -8;
  for (char c : s) {
    if (c == '=')
      break;
    uint8_t d = b64lut(c);
    if (d == 0xFF)
      return false;
    val = (val << 6) | d;
    valb += 6;
    if (valb >= 0) {
      out.push_back(char((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  s.swap(out);
  return true;
}

static inline void capitalize_first(std::string& s) {
  if (!s.empty() && s[0] >= 'a' && s[0] <= 'z')
    s[0] = char(s[0] - 'a' + 'A');
}
static bool parseHeaders(const std::string& headers, int& status,
                         std::map<std::string, std::string>& out) {
  // Status
  size_t line_end = headers.find("\r\n");
  if (line_end == std::string::npos)
    return false;
  auto first = headers.substr(0, line_end);
  size_t sp = first.find(' ');
  status = (sp != std::string::npos) ? atoi(first.c_str() + sp + 1) : 0;

  // Headers
  size_t pos = line_end + 2;
  while (pos < headers.size()) {
    size_t next = headers.find("\r\n", pos);
    if (next == std::string::npos || next == pos)
      break;
    auto line = headers.substr(pos, next - pos);
    pos = next + 2;
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string k = line.substr(0, colon);
      std::string v = line.substr(colon + 1);
      while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
        v.erase(v.begin());
      for (auto& ch : k)
        ch = (char)tolower((unsigned char)ch);
      out[k] = v;
    }
  }
  return true;
}

static bool readHttpResponse(bell::SocketStream& s, int& status,
                             std::map<std::string, std::string>& headers,
                             std::vector<uint8_t>& raw) {
  // Read header block
  std::string hdrs;
  hdrs.reserve(2048);
  while (s.rdbuf()->sgetc() != std::char_traits<char>::eof()) {
    char c = (char)s.rdbuf()->sbumpc();
    hdrs.push_back(c);
    size_t n = hdrs.size();
    if (n >= 4 && hdrs[n - 4] == '\r' && hdrs[n - 3] == '\n' &&
        hdrs[n - 2] == '\r' && hdrs[n - 1] == '\n')
      break;
    if (hdrs.size() > 64 * 1024)
      break;  // guard
  }
  if (!parseHeaders(hdrs, status, headers))
    return false;

  // Body to EOF
  char buf[1024];
  while (s.rdbuf()->sgetc() != std::char_traits<char>::eof()) {
    std::streamsize n = s.readsome(buf, sizeof(buf));
    if (n > 0)
      raw.insert(raw.end(), (uint8_t*)buf, (uint8_t*)buf + n);
    else
      break;
  }
  return true;
}

static inline int hexVal(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return 10 + c - 'a';
  if (c >= 'A' && c <= 'F')
    return 10 + c - 'A';
  return -1;
}
static std::string unescapeJSON(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c == '\\' && i + 1 < in.size()) {
      char n = in[++i];
      switch (n) {
        case '"':
          out.push_back('"');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case '/':
          out.push_back('/');
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          if (i + 4 < in.size()) {
            int v = (hexVal(in[i + 1]) << 12) | (hexVal(in[i + 2]) << 8) |
                    (hexVal(in[i + 3]) << 4) | (hexVal(in[i + 4]));
            if (v >= 0) {
              out.push_back((char)(v & 0x7F));
              i += 4;
            }
          }
          break;
        }
        default:
          out.push_back(n);
          break;
      }
    } else
      out.push_back(c);
  }
  return out;
}

void scan_app_id(const std::string& hay, std::string& app_id) {
  // Look for: production:{api:{appId:"#########"
  const char* key = "production:{api:{appId:\"";
  size_t p = hay.find(key);
  if (p == std::string::npos)
    return;
  SC32_LOG(info, "found appId at %d", p);
  p += std::strlen(key);
  // next 9 digits
  std::string id;
  while (p < hay.size() && std::isdigit((unsigned char)hay[p]) &&
         id.size() < 9) {
    id.push_back(hay[p++]);
  }
  if (id.size() == 9)
    app_id = id;
  SC32_LOG(info, "found appId %s", app_id.c_str());
}
struct Seed {
  std::string tz_cap;
  std::string seed;
};

void scan_seeds(const std::string& hay,
                std::map<std::string, std::string>& seeds) {
  // pattern:  x.initialSeed("SEED",window.utimezone.<tz>)
  const char* needle = ".initialSeed(\"";
  size_t i = 0;
  while ((i = hay.find(needle, i)) != std::string::npos) {
    SC32_LOG(info, "found initialSeed at %d", i);
    i += std::strlen(needle);
    size_t q = hay.find('"', i);
    if (q == std::string::npos)
      break;
    std::string seed = hay.substr(i, q - i);
    const char* tzp = ",window.utimezone.";
    size_t t = hay.find(tzp, q);
    if (t == std::string::npos)
      continue;
    t += std::strlen(tzp);
    size_t e = t;
    while (e < hay.size() && std::isalpha((unsigned char)hay[e]))
      ++e;
    std::string tz = hay.substr(t, e - t);  // e.g., "europe"
    capitalize_first(tz);                   // "Europe"
    seeds.emplace(tz, seed);
    SC32_LOG(info, "seed for %s: %s", tz.c_str(), seed.c_str());
    i = e;
  }
}

void scan_info_extras_and_derive(
    const std::string& hay, const std::map<std::string, std::string>& seeds,
    std::map<std::string, std::string>& tz_secret) {
  for (const auto& s : seeds) {
    // match: name:".../<Timezone[a-z]?>",info:"INFO",extras:"EXTRAS"
    // Build a light finder around the timezone anchor:
    std::string anchor =
        "/";            // any prefix before slash is fine; we only check suffix
    anchor += s.first;  // "Europe"
    size_t scan = 0;
    while ((scan = hay.find(anchor, scan)) != std::string::npos) {
      // grab info="...", extras="..."
      size_t info_k = hay.find("info:", scan);
      if (info_k != std::string::npos)
        SC32_LOG(info, "found info %s at %d", hay.c_str(), scan);
      size_t extras_k = hay.find("extras:", scan);
      if (extras_k != std::string::npos)
        SC32_LOG(info, "found extra %s at %d", hay.c_str(), scan);
      if (info_k == std::string::npos || extras_k == std::string::npos)
        break;
      size_t space_check = hay.find("info: ", scan);
      info_k += 6;
      extras_k += 8;
      if (space_check != std::string::npos) {
        info_k++;
        extras_k++;
      }
      size_t info_end = hay.find('"', info_k);
      size_t extras_end = hay.find('"', extras_k);
      if (info_end == std::string::npos || extras_end == std::string::npos) {
        info_end = hay.find('\'', info_k);
        extras_end = hay.find('\'', extras_k);
        if (info_end == std::string::npos || extras_end == std::string::npos)
          break;
      }
      std::string info = hay.substr(info_k, info_end - info_k);
      std::string extras = hay.substr(extras_k, extras_end - extras_k);

      // derive secret: base64url( (seed+info+extras) without last 44 chars )
      std::string chars = s.second + info + extras;
      if (chars.size() <= 44)
        break;
      std::string enc = chars.substr(0, chars.size() - 44);
      if (!base64url_decode_inplace(enc))
        break;
      // 'enc' now holds raw bytes; they used UTF-8 in the repo
      tz_secret.emplace(s.first, enc);
      SC32_LOG(info, "secret for %s: %s", s.first.c_str(), enc.c_str());
      break;  // one good hit per tz is enough
    }
  }
}

static void streamScanForQobuzSecrets(
    bell::SocketStream& s, QobuzConfig::ClientAppSecrets& web_secrets) {
  static const size_t WINSZ = 2048;  // sliding window size
  static const size_t CHUNK = 512;   // read size
  std::string win;
  win.reserve(WINSZ * 2);
  QobuzConfig::AppKey tempk;
  while (s.rdbuf()->sgetc() != std::char_traits<char>::eof()) {
    char buf[CHUNK];
    std::streamsize n = s.readsome(buf, sizeof(buf));
    if (n <= 0)
      break;
    win.append(buf, buf + n);
    if (win.size() > WINSZ * 2)
      win.erase(0, win.size() - WINSZ);  // keep tail
    if (web_secrets.id.empty())
      scan_app_id(win, web_secrets.id);
    scan_seeds(win, web_secrets.seeds);
    scan_info_extras_and_derive(win, web_secrets.seeds, web_secrets.secrets);
    if (web_secrets.secrets.size()) {
      if (web_secrets.secrets.size() == web_secrets.seeds.size()) {
        s.flush();
        return;
      }
    }
    // let the IDLE feed WDT
    BELL_SLEEP_MS(1);
  }
}

void scanBundleFullStream(const std::string& jsUrl,
                          QobuzConfig::ClientAppSecrets& out) {
  auto u = bell::URLParser::parse(jsUrl);
  bell::SocketStream s;
  if (s.open(u.host, u.port, u.schema == "https") != 0)
    throw std::runtime_error("open failed (full stream)");

  std::string path = u.path.empty() ? "/" : u.path;

  // HTTP/1.1, read-to-EOF, identity encoding
  std::string req;
  req += "GET " + path + " HTTP/1.1\r\n";
  req += "Host: " + u.host + "\r\n";
  req += "User-Agent: Mozilla/5.0\r\n";
  req += "Accept: */*\r\n";
  req += "Accept-Encoding: identity\r\n";
  req += "Connection: close\r\n";
  req += "Referer: https://play.qobuz.com/\r\n";
  req += "\r\n";
  s << req;
  s.flush();

  // skip headers
  std::string hdrs;
  hdrs.reserve(1024);
  while (s.rdbuf()->sgetc() != std::char_traits<char>::eof()) {
    char c = (char)s.rdbuf()->sbumpc();
    hdrs.push_back(c);
    size_t n = hdrs.size();
    if (n >= 4 && hdrs[n - 4] == '\r' && hdrs[n - 3] == '\n' &&
        hdrs[n - 2] == '\r' && hdrs[n - 1] == '\n')
      break;
    if (hdrs.size() > 64 * 1024)
      break;
  }

  // STREAM-SCAN the body with tiny sliding window (you already have this)
  streamScanForQobuzSecrets(s, out);
  s.close();
}

// tolerant dechunker (handles chunk extensions & trailers)
static bool dechunk_http_body(const std::vector<uint8_t>& in,
                              std::vector<uint8_t>& out) {
  out.clear();
  size_t i = 0, n = in.size();
  auto read_line = [&](std::string& line) -> bool {
    line.clear();
    while (i < n) {
      char c = (char)in[i++];
      if (c == '\r') {
        if (i < n && in[i] == '\n') {
          ++i;
          return true;
        }
        return false;
      }
      line.push_back(c);
      if (line.size() > 2048)
        return false;
    }
    return false;
  };

  while (i < n) {
    std::string szline;
    if (!read_line(szline))
      return false;
    size_t semi = szline.find(';');
    if (semi != std::string::npos)
      szline.resize(semi);
    char* endp = nullptr;
    long long sz = strtoll(szline.c_str(), &endp, 16);
    if (endp == szline.c_str() || sz < 0)
      return false;

    if (sz == 0) {
      std::string trailer;
      // swallow trailers until blank line (ok if missing)
      if (!read_line(trailer))
        return true;
      while (!trailer.empty()) {
        if (!read_line(trailer))
          break;
      }
      return true;
    }
    if (i + (size_t)sz > n)
      return false;
    out.insert(out.end(), in.begin() + i, in.begin() + i + (size_t)sz);
    i += (size_t)sz;
    if (i + 1 >= n || in[i] != '\r' || in[i + 1] != '\n')
      return false;
    i += 2;
  }
  return true;
}
static std::string absolutize(const std::string& base, const std::string& ref) {
  if (ref.rfind("http://", 0) == 0 || ref.rfind("https://", 0) == 0)
    return ref;
  auto b = bell::URLParser::parse(base);
  std::string hostport =
      b.host +
      ((b.port != 80 && b.port != 443) ? (":" + std::to_string(b.port)) : "");
  if (!ref.empty() && ref[0] == '/')
    return b.schema + "://" + hostport + ref;
  // relative
  auto path = b.path.empty() ? "/" : b.path;
  auto slash = path.find_last_of('/');
  auto dir = (slash == std::string::npos) ? "/" : path.substr(0, slash + 1);
  return b.schema + "://" + hostport + dir + ref;
}
struct HttpResult {
  int status = 0;
  std::string body;
  std::map<std::string, std::string> headers;
};
static std::vector<std::string> extractScriptSrcs(const std::string& html) {
  std::vector<std::string> out;
  size_t p = 0;
  auto push_if_js = [&](const std::string& url) {
    if (url.empty())
      return;
    if (url.rfind("data:", 0) == 0)
      return;
    if (url.find(".js") != std::string::npos)
      out.push_back(url);
  };

  // <script src="...">
  while ((p = html.find("<script", p)) != std::string::npos) {
    size_t tagEnd = html.find('>', p);
    if (tagEnd == std::string::npos)
      break;
    size_t s = html.find("src=", p);
    if (s != std::string::npos && s < tagEnd) {
      s += 4;
      if (s < html.size() && (html[s] == '"' || html[s] == '\'')) {
        char q = html[s++];
        size_t e = html.find(q, s);
        if (e != std::string::npos)
          push_if_js(html.substr(s, e - s));
      }
    }
    p = tagEnd == std::string::npos ? p + 7 : tagEnd + 1;
  }

  // <link rel="preload" as="script" href="...">
  p = 0;
  while ((p = html.find("<link", p)) != std::string::npos) {
    size_t tagEnd = html.find('>', p);
    if (tagEnd == std::string::npos)
      break;

    auto frag = html.substr(p, tagEnd - p + 1);
    // cheap contains checks
    auto low = frag;
    for (auto& c : low)
      c = (char)tolower((unsigned char)c);
    if (low.find("rel=\"preload\"") == std::string::npos &&
        low.find("rel='preload'") == std::string::npos) {
      p = tagEnd + 1;
      continue;
    }
    if (low.find("as=\"script\"") == std::string::npos &&
        low.find("as='script'") == std::string::npos) {
      p = tagEnd + 1;
      continue;
    }

    // get href
    size_t h = frag.find("href=");
    if (h != std::string::npos) {
      h += 5;
      if (h < frag.size() && (frag[h] == '"' || frag[h] == '\'')) {
        char q = frag[h++];
        size_t e = frag.find(q, h);
        if (e != std::string::npos)
          push_if_js(frag.substr(h, e - h));
      }
    }
    p = tagEnd + 1;
  }

  return out;
}

static bool isPlayerAsset(const std::string& base, const std::string& url) {
  // absolutize first so we can check host
  auto abs = absolutize(base, url);
  auto u = bell::URLParser::parse(abs);
  // only the player domain
  if (u.host != "play.qobuz.com")
    return false;
  // likely bundles live here
  if (u.path.find("/resources/") != std::string::npos)
    return true;
  if (u.path.find("/_next/") != std::string::npos)
    return true;  // future-proof
  if (u.path.rfind(".js") != std::string::npos)
    return true;  // fallback
  return false;
}

static std::vector<std::string> extractPlayerScriptSrcs(
    const std::string& html, const std::string& baseUrl) {
  std::vector<std::string> all =
      extractScriptSrcs(html);  // your existing function
  std::vector<std::string> out;
  out.reserve(all.size());
  for (auto& s : all) {
    if (isPlayerAsset(baseUrl, s))
      out.push_back(absolutize(baseUrl, s));
  }
  // de-dup
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

static HttpResult bellGet_follow(const std::string& startUrl,
                                 int max_redirs = 3) {
  std::string url = startUrl;
  for (int hop = 0; hop <= max_redirs; ++hop) {
    auto u = bell::URLParser::parse(url);  // fields: schema, host, path, port
    const bool isTLS = (u.schema == "https");
    bell::SocketStream s;
    if (s.open(u.host, u.port, isTLS) !=
        0) {  // SocketStream picks TLS vs TCP via bool flag
      throw std::runtime_error("open failed: " + url);
    }
    writeHttp11Get(s, u);
    int status = 0;
    std::map<std::string, std::string> hdrs;
    std::vector<uint8_t> raw;
    if (!readHttpResponse(s, status, hdrs, raw)) {
      s.close();
      throw std::runtime_error("read response failed");
    }
    s.close();

    // Handle redirects
    if (status >= 300 && status < 400) {
      auto it = hdrs.find("location");
      if (it != hdrs.end()) {
        url = absolutize(url, it->second);
        continue;
      }
    }

    HttpResult r;
    r.status = status;
    r.headers = std::move(hdrs);
    auto te = r.headers.find("transfer-encoding");
    if (te != r.headers.end() &&
        te->second.find("chunked") != std::string::npos) {
      std::vector<uint8_t> out;
      if (!dechunk_http_body(raw, out))
        throw std::runtime_error("Chunked decode error");
      r.body.assign((const char*)out.data(), out.size());
    } else {
      r.body.assign((const char*)raw.data(), raw.size());
    }
    return r;
  }
  throw std::runtime_error("too many redirects");
}

void tryQobuzFromBundlesBounded(const std::string& entryUrl,
                                QobuzConfig::ClientAppSecrets& secrets) {
  auto page =
      bellGet_follow(entryUrl);  // your robust GET+redirects, JSON headers
  auto scripts = extractPlayerScriptSrcs(
      page.body, entryUrl);  // <— filter to play.qobuz.com

  // short-circuit if page itself had inline config
  //pluckCommonSecrets(page.body, secrets);
  if (!secrets.empty())
    return;

  if (scripts.empty())
    return;

  size_t limit = std::min(scripts.size(), (size_t)12);
  for (size_t i = 0; i < limit; ++i) {
    const std::string& jsUrl = scripts[i];
    auto u = bell::URLParser::parse(jsUrl);
    if (u.host != "play.qobuz.com") {
      SC32_LOG(info, "wont scan JS[%u/%u]: %s", (unsigned)i + 1,
               (unsigned)limit, jsUrl.c_str());
      BELL_SLEEP_MS(1);
      continue;
    }
    SC32_LOG(info, "scan JS[%u/%u]: %s", (unsigned)i + 1, (unsigned)limit,
             jsUrl.c_str());
    BELL_SLEEP_MS(1);
    // bigger tail slice for player bundles: 128KB head + 384KB tail
    scanBundleFullStream(jsUrl, secrets);
  }
  return;
}

}  // namespace

QobuzConfig::ClientAppSecrets QobuzConfig::FetchClientAppSecrets() {

  QobuzConfig::ClientAppSecrets s;
  BELL_SLEEP_MS(1);

  // Prefer web-bundle scrape for Qobuz (api.json returns HTML in your locale)
  if (opts_.url.find("qobuz.com") != std::string::npos) {
    // hit the web player landing; login page has the same bundles
    static const char* qobuzEntrypoints[] = {"https://play.qobuz.com/login",
                                             "https://play.qobuz.com/"};
    for (auto ep : qobuzEntrypoints) {
      try {
        tryQobuzFromBundlesBounded(ep, s);
        if (!s.secrets.empty()) {
          SC32_LOG(info, "found %i secrets and %i api keys", s.secrets.size(),
                   s.api_keys.size());
          for (auto& k : s.api_keys)
            SC32_LOG(info, "api key: %s", k.c_str());
          for (auto& k : s.secrets)
            SC32_LOG(
                info, "app id: %.8s..., secret: %.8s...", s.id.c_str(),
                k.second
                    .c_str());  // SC32_LOG(info, "app id: %s", k.id.c_str());
          return s;
        }
      } catch (const std::bad_alloc&) {
        SC32_LOG(error, "OOM while scanning bundles. Lower slice sizes.");
        // degrade: firstKB=64, tailKB=64 or even 32
      }
    }
  }
  return s;
}