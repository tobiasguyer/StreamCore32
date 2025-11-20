#include "HTTPClient.h"

#include <string.h>   // for memcpy
#include <algorithm>  // for transform
#include <algorithm>
#include <cassert>    // for assert
#include <cctype>     // for tolower
#include <ostream>    // for operator<<, basic_ostream
#include <stdexcept>  // for runtime_error

#include <chrono>  // for duration_cast, steady_clock

#include "BellLogger.h"  // for AbstractLogger, BELL_LOG
#include "BellSocket.h"  // for bell
#include "BellUtils.h"   // for BELL_SLEEP_MS

using namespace bell;
using namespace std::chrono;

static inline long long nowMs() {
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::string HTTPClient::CookieJar::toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
  return s;
}

bool HTTPClient::CookieJar::domainMatches(const std::string& host, const std::string& cookieDomain) {
  if (cookieDomain.empty()) return false;
  // Exact
  if (toLower(host) == toLower(cookieDomain)) return true;
  // Leading dot or suffix match
  if (cookieDomain.front() == '.') {
    auto d = toLower(cookieDomain.substr(1));
    auto h = toLower(host);
    return h.size() >= d.size() && (h == d || (h.size()>d.size() && h.rfind("." + d) == h.size()- (d.size()+1)));
  }
  // Suffix match as per RFC (simplified)
  auto d = toLower(cookieDomain);
  auto h = toLower(host);
  return h.size() > d.size() && h.rfind("." + d) == h.size() - (d.size()+1);
}

bool HTTPClient::CookieJar::pathMatches(const std::string& reqPath, const std::string& cookiePath) {
  if (cookiePath.empty()) return true;
  if (cookiePath == "/") return true;
  if (reqPath.rfind(cookiePath, 0) == 0) return true; // prefix
  return false;
}

void HTTPClient::CookieJar::ingestSetCookieHeaders(const std::vector<std::pair<std::string,std::string>>& headers,
                                                   const bell::URLParser& url) {
  // collect all Set-Cookie headers
  for (auto& kv : headers) {
    std::string key = toLower(kv.first);
    if (key != "set-cookie") continue;
    BELL_LOG(debug, "QOBUZ", "Set-Cookie: %s", kv.second.c_str());
    std::string v = kv.second;

    // split into attributes: NAME=VALUE; Attr=...; Secure; Path=...; Domain=...; Max-Age=...
    // take first segment as name=value
    size_t semi = v.find(';');
    std::string nv = trim(semi == std::string::npos ? v : v.substr(0, semi));
    if (nv.empty()) continue;
    size_t eq = nv.find('=');
    if (eq == std::string::npos) continue;

    Cookie c;
    c.name  = trim(nv.substr(0, eq));
    c.value = trim(nv.substr(eq+1));
    c.domain = url.host; // default to request host
    c.path = "/";        // default
    c.setAtMs = nowMs();
    c.maxAge = -1;
    c.secure = false;

    // parse attributes
    size_t pos = semi;
    while (pos != std::string::npos && pos < v.size()) {
      size_t next = v.find(';', pos+1);
      std::string attr = trim(v.substr(pos+1, (next==std::string::npos? v.size(): next) - (pos+1)));
      pos = next;
      if (attr.empty()) continue;

      size_t aeq = attr.find('=');
      std::string akey = toLower(trim(aeq==std::string::npos ? attr : attr.substr(0, aeq)));
      std::string aval = (aeq==std::string::npos)? "" : trim(attr.substr(aeq+1));

      if (akey == "domain" && !aval.empty()) c.domain = aval;
      else if (akey == "path" && !aval.empty()) c.path = aval;
      else if (akey == "max-age" && !aval.empty()) {
        char* end=nullptr; long long s = std::strtoll(aval.c_str(), &end, 10);
        if (end!=aval.c_str()) c.maxAge = s;
      }
      else if (akey == "secure") c.secure = true;
      // (HttpOnly ignored here because we’re not a browser JS env)
      // (Expires omitted to stay tiny)
    }

    // upsert into store under normalized domain key (lowercase)
    auto domKey = toLower(c.domain);
    auto& vec = store[domKey];

    bool replaced = false;
    for (auto& oldc : vec) {
      if (toLower(oldc.name)==toLower(c.name) && toLower(oldc.path)==toLower(c.path) && toLower(oldc.domain)==toLower(c.domain)) {
        oldc = c;
        replaced = true;
        break;
      }
    }
    if (!replaced) vec.push_back(std::move(c));
  }
}

std::string HTTPClient::CookieJar::cookieHeaderFor(const bell::URLParser& url) const {
  const bool isHttps = (url.schema == "https");
  std::string hostL = toLower(url.host);
  std::string path = url.path.empty()? "/" : url.path;

  // collect matching cookies (and drop expired in-flight)
  std::vector<std::pair<std::string,std::string>> pairs;

  auto itExact = store.find(hostL);
  auto consider = [&](const std::vector<Cookie>& v){
    for (auto& c : v) {
      // expiry
      if (c.maxAge >= 0) {
        long long ageMs = nowMs() - c.setAtMs;
        if (ageMs > (c.maxAge * 1000)) continue;  // expired
      }
      // secure
      if (c.secure && !isHttps) continue;
      // domain + path match
      if (!domainMatches(url.host, c.domain)) continue;
      if (!pathMatches(path, c.path)) continue;

      pairs.emplace_back(c.name, c.value);
    }
  };

  if (itExact != store.end()) consider(itExact->second);

  // also try suffix domains (e.g. ".qobuz.com") present in the jar
  for (auto& kv : store) {
    if (kv.first == hostL) continue;
    // skip unrelated hosts quickly
    if (!domainMatches(url.host, kv.first)) continue;
    consider(kv.second);
  }

  if (pairs.empty()) return {};

  // RFC: order not critical here; join as "name=value; name2=value2"
  std::string out;
  for (size_t i=0;i<pairs.size();++i) {
    if (i) out += "; ";
    out += pairs[i].first;
    out += '=';
    out += pairs[i].second;
  }
  return out;
}

int HTTPClient::Response::connect(const std::string& url, size_t numHeaders) {
    urlParser = bell::URLParser::parse(url);
    maxHeaders = numHeaders;
    delete[] phResponseHeaders;  // Ensure no memory leaks if reallocating
    phResponseHeaders = new phr_header[maxHeaders];
    return this->socketStream.open(urlParser.host, urlParser.port,
                                   urlParser.schema == "https");

}

int HTTPClient::Response::reconnect() {
    if (this->socketStream.isOpen()) {
      this->socketStream.flush();
      this->socketStream.close();
    }
    BELL_SLEEP_MS(10);
    return this->socketStream.open(urlParser.host, urlParser.port,
                                   urlParser.schema == "https");
}

HTTPClient::Response::~Response() {
  delete[] phResponseHeaders;  // Free the dynamically allocated header array
  if (this->socketStream.isOpen()) {
    //this->drainBody();
    this->socketStream.close();
  }
}

bool HTTPClient::Response::rawRequest(const std::string& url,
                                      const std::string& method,
                                      const std::vector<uint8_t>& content,
                                      Headers& headers, bool keepAlive) {
  urlParser = bell::URLParser::parse(url);

  const char* reqEnd = "\r\n";
  const int kMaxAttempts = 3;
  uint8_t attempts = kMaxAttempts;
  do {
    responseHeaders.clear();
    rawBody.clear();
    httpBufferAvailable = 0;
    statusCode = 0;
    if(!socketStream.isOpen()) {
      if (connect(url, maxHeaders) != 0) {
        BELL_LOG(info, "httpClient","Failed to open socket");
        return false;
      }
    }
  // Prepare a request

// fix: omit default ports (80/443)
const bool isHttps = (urlParser.schema == "https");
const bool defaultPort = ( ( !isHttps && urlParser.port == 80 )
                        || (  isHttps && urlParser.port == 443) );
    socketStream << method << " " << urlParser.path << " HTTP/1.1" << reqEnd;
    socketStream << "Host: " << urlParser.host;
    if (!defaultPort && urlParser.port > 0) {
      socketStream << ":" << urlParser.port;
    }
    socketStream << reqEnd;
    if(!keepAlive) {
      socketStream << "Connection: close" << reqEnd;
    }
    else {
      socketStream << "Connection: keep-alive" << reqEnd;
    }
    
    socketStream << "Accept: */*" << reqEnd;

    // Write content
    if (!content.empty()) {
      socketStream << "Content-Length: " << content.size() << reqEnd;
    }
    // If we have a cookie jar, emit Cookie:
    if (cookieJar_) {
      std::string cookieVal = cookieJar_->cookieHeaderFor(urlParser);
      if (!cookieVal.empty()) {
        socketStream << "Cookie: " << cookieVal << reqEnd;
      }
    }
    // Write headers
    for (auto& header : headers) {
      socketStream << header.first << ": " << header.second << reqEnd;
    }

    socketStream << reqEnd;

    // Write request body if it exists
    if (!content.empty()) {
      socketStream.write(reinterpret_cast<const char*>(content.data()),
                         content.size());
    }

    socketStream.flush();
  // Parse response
    if(readResponseHeaders()) return true;
    // failed to get headers, drop socket and try again
    socketStream.close();
    if(--attempts) BELL_SLEEP_MS(75);
  } while (attempts > 0);
  return false;
}

void HTTPClient::Response::drainBody(uint32_t max_ms) {
  const uint64_t deadline = nowMs() + max_ms;
  std::array<char, 4096> buf;

  auto avail_now = [&]()->std::streamsize {
    // works after (1): reports TLS buffered bytes without blocking
    return (std::streamsize)this->socketStream.rdbuf()->in_avail();
  };
  // 1) Content-Length
  if (hasContentSize) {
    size_t left = contentSize;
    while (left && nowMs() < deadline) {
      auto avail = avail_now();
      if (avail <= 0) break;                         // no data ready; don't block
      auto toRead = std::min<size_t>(left, std::min<size_t>(buf.size(), (size_t)avail));
      this->socketStream.read(buf.data(), toRead);   // will not block given avail>0
      left -= (size_t)this->socketStream.gcount();
      if (this->socketStream.gcount() == 0) break;   // no progress
    }
    return;
  }

  // 2) Chunked transfer
  auto isChunked = [&]{
    std::string v(header("transfer-encoding"));
    std::transform(v.begin(), v.end(), v.begin(), ::tolower);
    return v.find("chunked") != std::string::npos;
  };

  if (isChunked()) {
    phr_chunked_decoder dec{};
    dec.consume_trailer = 1;

    std::string scratch; scratch.reserve(8192);
    while (nowMs() < deadline) {
      auto avail = avail_now();
      if (avail <= 0) break;

      auto toRead = (size_t)std::min<std::streamsize>(avail, (std::streamsize)buf.size());
      this->socketStream.read(buf.data(), toRead);
      size_t n = (size_t)this->socketStream.gcount();
      if (n == 0) break;

      scratch.append(buf.data(), n);

      size_t bufsz = scratch.size();
      ssize_t r = phr_decode_chunked(&dec, scratch.data(), &bufsz);
      if (r == -1) break;         // decode error -> bail out (don’t block)
      // drop decoded bytes; we’re just draining
      scratch.erase(0, bufsz);
      if (r >= 0) break;          // finished chunked body (trailers consumed)
    }
    return;
  }

  // 3) Neither CL nor chunked => read to EOF but only while bytes are buffered
  while (nowMs() < deadline) {
    auto avail = avail_now();
    if (avail <= 0) break;
    auto toRead = (size_t)std::min<std::streamsize>(avail, buf.size());
    this->socketStream.read(buf.data(), toRead);
    if (this->socketStream.gcount() == 0) break;
  }
}

bool HTTPClient::Response::readResponseHeaders() {
  const char* msgPointer = nullptr;
  size_t msgLen = 0;
  int minorVersion = 0, status = 0;

  size_t prevbuflen = 0;
  size_t numHeaders = maxHeaders;
  httpBufferAvailable = 0;

  const int MAX_WAIT_MS = 5000;
  int waited_ms = 0;

  while (true) {
    if (httpBufferAvailable == httpBuffer.size()) {
      BELL_LOG(error, "httpClient", "HTTP header buffer overflow");
      return false;
    }

    // Read a chunk (not the whole free space!)
    size_t want = httpBuffer.size() - httpBufferAvailable;
    if (want > 512) want = 512;  // small bites keep latency low

    ssize_t n = socketStream.readSome(
        reinterpret_cast<char*>(httpBuffer.data()) + httpBufferAvailable, want);

    if (n > 0) {
      prevbuflen = httpBufferAvailable;
      httpBufferAvailable += static_cast<size_t>(n);

      numHeaders = maxHeaders;
      int pret = phr_parse_response(
          reinterpret_cast<const char*>(httpBuffer.data()), httpBufferAvailable,
          &minorVersion, &status, &msgPointer, &msgLen,
          phResponseHeaders, &numHeaders, prevbuflen);

      if (pret > 0) {
        // Success — copy headers and set content length (if present)
        responseHeaders.clear();
        for (size_t i = 0; i < numHeaders; ++i) {
          responseHeaders.emplace_back(
              std::string(phResponseHeaders[i].name,  phResponseHeaders[i].name_len),
              std::string(phResponseHeaders[i].value, phResponseHeaders[i].value_len));
        }
        statusCode = status;
        auto cl = header("content-length");
        hasContentSize = !cl.empty();
        contentSize = hasContentSize ? static_cast<size_t>(std::strtoul(std::string(cl).c_str(), nullptr, 10)) : 0;

        // If we already have body bytes in buffer, keep them
        size_t bodyOffset = static_cast<size_t>(pret);
        if (httpBufferAvailable > bodyOffset) {
          size_t extra = httpBufferAvailable - bodyOffset;
          if (extra) {
            rawBody.insert(rawBody.end(),
                           httpBuffer.begin() + bodyOffset,
                           httpBuffer.begin() + httpBufferAvailable);
          }
        }// Feed Set-Cookie headers to the jar
        if (cookieJar_) {
          cookieJar_->ingestSetCookieHeaders(responseHeaders, urlParser);
        }
        return true;
      } else if (pret == -1) {
        BELL_LOG(error, "httpClient", "HTTP header parse error");
        return false;
      } else {
        // -2: incomplete → continue; reset wait timer since we made progress
        waited_ms = 0;
      }
    } else if (n == 0) {
      BELL_LOG(error, "httpClient", "Peer closed before headers (will retry)");
      return false;
    } else { 
      BELL_LOG(error, "httpClient", "Socket/TLS read error (will retry)");
      return false;
    }

    // backoff to avoid watchdog if no new data yet
    BELL_SLEEP_MS(10);
    waited_ms += 10;
    if (waited_ms >= MAX_WAIT_MS) return false;  // let caller reconnect
  }
}


bool HTTPClient::Response::get(const std::string& url, Headers headers, bool keepAlive) {
  return this->rawRequest(url, "GET", {}, headers, keepAlive);
}

bool HTTPClient::Response::post(const std::string& url, Headers headers,
                                const std::vector<uint8_t>& body, bool keepAlive) {
  return this->rawRequest(url, "POST", body, headers, keepAlive);
}

size_t HTTPClient::Response::contentLength() {
  return contentSize;
}

std::string_view HTTPClient::Response::header(const std::string& headerName) {
  for (auto& header : this->responseHeaders) {
    std::string headerValue = header.first;
    std::transform(headerValue.begin(), headerValue.end(), headerValue.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (headerName == headerValue) {
      const auto& value = header.second;
      return std::string_view(value);
    }
  }

  return "";
}

size_t HTTPClient::Response::totalLength() {
  auto rangeHeader = header("content-range");

  if (rangeHeader.find("/") != std::string::npos) {
    return std::stoull(
        std::string(rangeHeader.substr(rangeHeader.find("/") + 1)));
  }

  return this->contentLength();
}
// --- naive dechunker (tolerant) ---
static bool dechunk_http_body(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
  out.clear();
  size_t i = 0, n = in.size();
  auto read_line = [&](std::string& line)->bool{
    line.clear();
    while (i < n) {
      char c = (char)in[i++];
      if (c == '\r') {
        if (i < n && in[i] == '\n') { ++i; return true; }
        return false; // malformed
      }
      line.push_back(c);
      if (line.size() > 1024) return false; // guard
    }
    return false;
  };

  while (i < n) {
    std::string szline;
    if (!read_line(szline)) return false;
    // trim any chunk extensions: "1a3;foo=bar"
    size_t semi = szline.find(';');
    if (semi != std::string::npos) szline.resize(semi);
    // hex size
    char* endp = nullptr;
    long long sz = strtoll(szline.c_str(), &endp, 16);
    if (endp == szline.c_str() || sz < 0) return false;

    if (sz == 0) {
      // consume trailing header lines until blank line
      std::string trailer;
      if (!read_line(trailer)) return true; // tolerate missing final CRLF
      while (!trailer.empty()) {
        if (!read_line(trailer)) break;
      }
      return true;
    }

    if (i + (size_t)sz > n) return false; // incomplete; caller should append more bytes first
    out.insert(out.end(), in.begin() + i, in.begin() + i + (size_t)sz);
    i += (size_t)sz;

    // expect CRLF after chunk
    if (i + 1 >= n || in[i] != '\r' || in[i+1] != '\n') return false;
    i += 2;
  }
  return true;
}

void HTTPClient::Response::readRawBody() {
  // If chunked, decode; else use Content-Length
  auto te = header("transfer-encoding");
  bool isChunked = (!te.empty() && std::string(te).find("chunked") != std::string::npos);

  if (isChunked) {
    BELL_LOG(debug, "httpClient", "chunked body");
    // Fast path: pico’s decoder
    std::vector<uint8_t> buf;
    buf.swap(rawBody);  // any post-header bytes go here

    phr_chunked_decoder dec{};
    dec.consume_trailer = 1;

    int idle_ms = 0;
    while (true) {
      size_t bufsz = buf.size();
      ssize_t r = phr_decode_chunked(&dec, reinterpret_cast<char*>(buf.data()), &bufsz);
      if (r >= 0) {  // complete
        rawBody.assign(buf.begin(), buf.begin() + bufsz);
        return;
      }
      if (r == -1) {
        // Fallback: read-to-EOF and manual dechunk
        // 1) slurp the rest
        uint8_t tmp[1024];
        while (true) {
          socketStream.read(reinterpret_cast<char*>(tmp), sizeof(tmp));
          std::streamsize got = socketStream.gcount();
          if (got <= 0) break;
          buf.insert(buf.end(), tmp, tmp + got);
        }
        // 2) try tolerant dechunker
        std::vector<uint8_t> out;
        if (dechunk_http_body(buf, out)) {
          rawBody.swap(out);
          return;
        }
        // 3) give up explicitly
        throw std::runtime_error("Chunked decode error");
      }
      // need more data
      uint8_t tmp[1024];
      socketStream.read(reinterpret_cast<char*>(tmp), sizeof(tmp));
      std::streamsize got = socketStream.gcount();
      if (got <= 0) {
        BELL_SLEEP_MS(5);
        idle_ms += 5;
        if (idle_ms >= 5000) { throw std::underflow_error("Timeout reading chunked body"); }
        continue;
      }
      idle_ms = 0;
      buf.insert(buf.end(), tmp, tmp + got);
    }
  }

  // Fixed-length body
  if (!hasContentSize || contentSize == 0) return;

  int idle_ms = 0;
  while (rawBody.size() < contentSize) {
    size_t need  = contentSize - rawBody.size();
    uint8_t tmp[1024];
    size_t ask   = need < sizeof(tmp) ? need : sizeof(tmp);

    socketStream.read(reinterpret_cast<char*>(tmp), ask);
    std::streamsize got = socketStream.gcount();
    if (got <= 0) {
      BELL_SLEEP_MS(5);
      idle_ms += 5;
      if (idle_ms >= 5000) { throw std::underflow_error("Timeout reading body"); }
      continue;
    }
    idle_ms = 0;
    rawBody.insert(rawBody.end(), tmp, tmp + got);
  }
}

std::string_view HTTPClient::Response::body() {
  readRawBody();
  return std::string_view((char*)rawBody.data(), rawBody.size());
}

std::vector<uint8_t> HTTPClient::Response::bytes() {
  readRawBody();
  return rawBody;
}
std::string HTTPClient::Response::body_string() {
  readRawBody();
  return std::string(reinterpret_cast<const char*>(rawBody.data()),
                     rawBody.size());
}