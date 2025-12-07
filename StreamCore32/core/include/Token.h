#pragma once
#include "EspRandomEngine.h"
#include "TimeSync.h"

class Token {
 public:
  Token() {
    generate();
    hex32 = hex32_from16(raw);
    b64 = b64url(raw, 16);
  }

  Token(const uint8_t b[16]) {
    memcpy(raw, b, 16);
    hex32 = hex32_from16(raw);
    b64 = b64url(raw, 16);
  }

  uint8_t* getRawPtr() { return raw; }
  std::string getRaw() { return std::string((const char*)raw, 16); }
  std::string getHex32() { return hex32; }
  std::string getB64() { return b64; }

 private:
  uint8_t raw[16];
  std::string hex32;  // for logs / hex style
  std::string b64;    // for logs / 22-char style

  void generate() {
    streamcore::esp_random_engine re;
    std::generate_n(raw, 16, re);
  }

  static std::string hex32_from16(const uint8_t b[16]) {
    static const char* H = "0123456789abcdef";
    std::string s;
    s.resize(32);
    for (int i = 0; i < 16; i++) {
      s[2 * i] = H[b[i] >> 4];
      s[2 * i + 1] = H[b[i] & 0xF];
    }
    return s;
  }
  static std::string b64url(const uint8_t* s, size_t n) {
    static const char* B =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    size_t i = 0;
    while (i < n) {
      uint32_t v = uint32_t(s[i++]) << 16;
      bool have2 = (i < n);
      if (have2)
        v |= uint32_t(s[i++]) << 8;
      bool have3 = (i < n);
      if (have3)
        v |= uint32_t(s[i++]);

      out.push_back(B[(v >> 18) & 63]);
      out.push_back(B[(v >> 12) & 63]);
      out.push_back(have2 ? B[(v >> 6) & 63] : '\0');
      out.push_back(have3 ? B[(v) & 63] : '\0');

      if (!have2) {
        out.pop_back();
        out.pop_back();
      }  // 1 byte => 2 chars
      else if (!have3) {
        out.pop_back();
      }  // 2 bytes => 3 chars
    }
    return out;
  }
};