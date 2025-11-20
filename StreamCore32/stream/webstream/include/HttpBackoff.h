#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include "HTTPClient.h"
#include "Logger.h"
#include "StreamBase.h"

namespace streamcore::helpers {

inline std::unique_ptr<bell::HTTPClient::Response>
httpGetWithBackoff(const std::string& url,
                   bell::HTTPClient::Headers headers,
                   uint32_t timeoutSec,
                   std::atomic<bool>* stopFlag = nullptr)
{
  constexpr int kMaxAttempts = 4; uint32_t backoffMs = 1000;
  headers.push_back({"User-Agent", "StreamCore32/Radio (ESP-IDF/Bell)"});
  for (int attempt=0; attempt<kMaxAttempts; ++attempt) {
    auto resp = bell::HTTPClient::get(url, headers, timeoutSec);
    if (resp) {
      auto retry = StreamBase::svToString(resp->header("Retry-After")); if (retry.empty()) retry = StreamBase::svToString(resp->header("retry-after"));
      auto rem   = StreamBase::svToString(resp->header("X-Rate-Limit-Remaining")); if (rem.empty()) rem = StreamBase::svToString(resp->header("x-rate-limit-remaining"));
      auto reset = StreamBase::svToString(resp->header("X-Rate-Limit-Reset")); if (reset.empty()) reset = StreamBase::svToString(resp->header("x-rate-limit-reset"));
      uint32_t waitSec = 0;
      if (!retry.empty()) waitSec = StreamBase::parseUint(retry);
      else if (!rem.empty() && StreamBase::parseUint(rem) == 0u) waitSec = std::max<uint32_t>(10, StreamBase::parseUint(reset));
      if (waitSec > 0) {
        uint32_t ms = std::min<uint32_t>(waitSec*1000u, 10u*60u*1000u);
        while (ms) { uint32_t s = std::min<uint32_t>(ms, 250); StreamBase::sleepMs(s); ms-=s; if (stopFlag && stopFlag->load()) return resp; }
        if (attempt + 1 == kMaxAttempts) return resp; // surface last
        continue;
      }
      return resp;
    }
    if (attempt + 1 == kMaxAttempts) return nullptr;
    uint32_t ms = backoffMs; while (ms) { uint32_t s=std::min<uint32_t>(ms,250); StreamBase::sleepMs(s); ms-=s; if (stopFlag && stopFlag->load()) break; }
    backoffMs = std::min<uint32_t>(backoffMs<<1, 8000u);
  }
  return nullptr;
}

} // namespace