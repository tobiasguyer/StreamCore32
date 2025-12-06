#include "QobuzSign.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <mbedtls/md5.h>

namespace qobuz {

  static std::string to_hex_lower(const uint8_t* buf, size_t len) {
    static const char* HEX = "0123456789abcdef";
    std::string out; out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
      out[2 * i] = HEX[(buf[i] >> 4) & 0xF];
      out[2 * i + 1] = HEX[(buf[i]) & 0xF];
    }
    return out;
  }

  static std::string concat_sorted_key_value(
    std::vector<std::pair<std::string, std::string>> kv)
  {
    std::sort(kv.begin(), kv.end(),
      [](auto& a, auto& b) { return a.first < b.first; });
    std::string s;
    s.reserve(128);
    for (auto& p : kv) { s += p.first; s += p.second; }
    return s;
  }

  static std::string url_encode(const std::string& in) {
    std::string out; out.reserve(in.size() * 3);
    auto is_unreserved = [](unsigned char c) {
      return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~';
      };
    char buf[4];
    for (unsigned char c : in) {
      if (is_unreserved(c)) out.push_back(c);
      else {
        std::snprintf(buf, sizeof(buf), "%%%02X", c);
        out += buf;
      }
    }
    return out;
  }

  std::string maybe_unpack_secret(const std::string& secret) {
    // If the secret is plain, return as-is.
    return secret;
  }

  std::string md5_sig(const std::string& object,
    const std::string& method,
    const std::vector<std::pair<std::string, std::string>>& params,
    const std::string& ts_text,
    const std::string& app_secret)
  {
    const std::string n = concat_sorted_key_value(params);          // key+value+...
    const std::string packed = object + method + n + ts_text + app_secret;

    uint8_t digest[16];
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);  // if your toolchain requires *_ret variants, see note below
    mbedtls_md5_update(&ctx, reinterpret_cast<const unsigned char*>(packed.data()),
      packed.size());
    mbedtls_md5_finish(&ctx, digest);
    mbedtls_md5_free(&ctx);

    return to_hex_lower(digest, sizeof(digest));
  }

  std::string build_query(const std::vector<std::pair<std::string, std::string>>& params)
  {
    std::string q;
    for (size_t i = 0;i < params.size();++i) {
      if (i) q.push_back('&');
      q += url_encode(params[i].first);
      q.push_back('=');
      q += url_encode(params[i].second);
    }
    return q;
  }

} // namespace qbz
