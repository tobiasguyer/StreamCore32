#pragma once

#include <stddef.h>     // for size_t
#include <cstdint>      // for uint8_t, int32_t
#include <memory>       // for make_unique, unique_ptr
#include <string>       // for string
#include <string_view>  // for string_view
#include <utility>      // for pair
#include <vector>       // for vector
#include <unordered_map>
#include <chrono>

#include "SocketStream.h"  // for SocketStream
#include "URLParser.h"     // for URLParser
#ifndef BELL_DISABLE_FMT
#include "fmt/core.h"  // for format
#endif
#include "picohttpparser.h"  // for phr_header

namespace bell {

class HTTPClient {
 public:
  // most basic header type, represents by a key-val
  typedef std::pair<std::string, std::string> ValueHeader;

  typedef std::vector<ValueHeader> Headers;

  // Helper over ValueHeader, formatting a HTTP bytes range
  struct RangeHeader {
    static ValueHeader range(size_t from, size_t to) {
#ifndef BELL_DISABLE_FMT
      return ValueHeader{"Range", fmt::format("bytes={}-{}", from, to)};
#else
      return ValueHeader{
          "Range", "bytes=" + std::to_string(from) + "-" + std::to_string(to)};
#endif
    }

    static ValueHeader last(size_t nbytes) {
#ifndef BELL_DISABLE_FMT
      return ValueHeader{"Range", fmt::format("bytes=-{}", nbytes)};
#else
      return ValueHeader{"Range", "bytes=-" + std::to_string(nbytes)};
#endif
    }
    static ValueHeader open(size_t from = 0) {
#ifndef BELL_DISABLE_FMT
      return ValueHeader{"Range", fmt::format("bytes={}-", from)};
#else
      return ValueHeader{"Range", "bytes=0-"};
#endif
    }
  };
// --- Simple CookieJar -------------------------------------------------
struct CookieJar {
  struct Cookie {
    std::string name, value, domain, path = "/";
    bool secure = false;
    // session cookie by default; we only keep Max-Age (seconds) for simplicity
    // (Expires parsing omitted to stay tiny)
    long long maxAge = -1; // -1 = session cookie
    long long setAtMs = 0; // monotonic-ish timestamp in ms (we'll store std::chrono::steady_clock)
  };

  // host -> vector of cookies
  std::unordered_map<std::string, std::vector<Cookie>> store;


  // Parse all Set-Cookie headers and store cookies for this URL
  void ingestSetCookieHeaders(const std::vector<std::pair<std::string,std::string>>& headers,
                              const bell::URLParser& url);

  // Build the "Cookie:" header value for this request URL
  std::string cookieHeaderFor(const bell::URLParser& url) const;

private:
  static bool domainMatches(const std::string& host, const std::string& cookieDomain);
  static bool pathMatches(const std::string& reqPath, const std::string& cookiePath);
  static std::string toLower(std::string s);
};

  class Response {
   public:
    Response(size_t numHeaders = 32)
        : phResponseHeaders(new phr_header[numHeaders]),
          maxHeaders(numHeaders) {}
    ~Response();

    /**
    * Initializes a connection with a given url.
    */
    int connect(const std::string& url, size_t numHeaders = 32);
    int reconnect();

    bool rawRequest(const std::string& url, const std::string& method,
                    const std::vector<uint8_t>& content, Headers& headers, bool keepAlive);
    bool get(const std::string& url, Headers headers = {}, bool keepAlive = true);
    bool post(const std::string& url, Headers headers = {},
              const std::vector<uint8_t>& body = {}, bool keepAlive = false);
    void setCookieJar(CookieJar* jar) { cookieJar_ = jar; }
    int status() { return this->statusCode; }

    std::string_view body();
    std::string      body_string(); 
    std::vector<uint8_t> bytes();

    std::string_view header(const std::string& headerName);
    bell::SocketStream& stream() { return this->socketStream; }

    size_t contentLength();
    size_t totalLength();

    bool isChunked() {
      std::string v(header("transfer-encoding"));
      std::transform(v.begin(), v.end(), v.begin(), ::tolower);
      return v.find("chunked") != std::string::npos;
    }
    Headers& headers() { return this->responseHeaders; }

void drainBody(uint32_t max_ms = 200);
   private:
    bell::URLParser urlParser;
    bell::SocketStream socketStream;

    Headers responseHeaders;
    phr_header* phResponseHeaders;
    size_t maxHeaders;  // Store max headers to handle numHeaders limit
    const size_t HTTP_BUF_SIZE = 4096;

    std::vector<uint8_t> httpBuffer = std::vector<uint8_t>(HTTP_BUF_SIZE);
    std::vector<uint8_t> rawBody = std::vector<uint8_t>();
    size_t httpBufferAvailable;

    size_t contentSize = 0;
    uint8_t retries__ = 5;
    int statusCode = 0;
    bool hasContentSize = false;

    CookieJar* cookieJar_ = nullptr;

    bool readResponseHeaders();
    void readRawBody();
  };

  enum class Method : uint8_t { GET = 0, POST = 1 };

  struct Request {
    std::string url;
    Method method;
    Headers headers;
  };

  static std::unique_ptr<Response> get(const std::string& url,
                                       Headers headers = {}, bool keepAlive = true,
                                       size_t numHeaders = 32) {
    auto response = std::make_unique<Response>(numHeaders);
    response->connect(url, numHeaders);
      response->get(url, headers, keepAlive);
    return response;
  }

  static std::unique_ptr<Response> post(const std::string& url,
                                        Headers headers = {},
                                        const std::vector<uint8_t>& body = {}, bool keepAlive = false, 
                                        size_t numHeaders = 32) {
    auto response = std::make_unique<Response>(numHeaders);
    response->connect(url, numHeaders);
      response->post(url, headers, body, keepAlive);
    return response;
  }
};

}  // namespace bell