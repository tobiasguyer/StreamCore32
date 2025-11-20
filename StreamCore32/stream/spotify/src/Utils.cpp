#include "Utils.h"

#include <stdlib.h>  // for strtol
#include <chrono>
#include <iomanip>      // for operator<<, setfill, setw
#include <iostream>     // for basic_ostream, hex
#include <sstream>      // for stringstream
#include <string>       // for string
#include <type_traits>  // for enable_if<>::type
#include <algorithm>   // nötig für std::find_if
#include <mbedtls/base64.h>
#ifndef _WIN32
#include <arpa/inet.h>
#endif

static std::string Base62Alphabet =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
static char Base64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

unsigned long long getCurrentTimestamp() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

uint64_t hton64(uint64_t value) {
  int num = 42;
  if (*(char*)&num == 42) {
    uint32_t high_part = htonl((uint32_t)(value >> 32));
    uint32_t low_part = htonl((uint32_t)(value & 0xFFFFFFFFLL));
    return (((uint64_t)low_part) << 32) | high_part;
  } else {
    return value;
  }
}

std::string base64Encode(const std::vector<uint8_t>& v) {
  std::string ss;
  int nLenOut = 0;
  int index = 0;
  while (index < v.size()) {
    size_t decode_size = v.size() - index;
    unsigned long n = v[index];
    n <<= 8;
    n |= (decode_size > 1) ? v[index + 1] : 0;
    n <<= 8;
    n |= (decode_size > 2) ? v[index + 2] : 0;
    uint8_t m4 = n & 0x3f;
    n >>= 6;
    uint8_t m3 = n & 0x3f;
    n >>= 6;
    uint8_t m2 = n & 0x3f;
    n >>= 6;
    uint8_t m1 = n & 0x3f;

    ss.push_back(Base64Alphabet[m1]);
    ss.push_back(Base64Alphabet[m2]);
    ss.push_back(decode_size > 1 ? Base64Alphabet[m3] : '=');
    ss.push_back(decode_size > 2 ? Base64Alphabet[m4] : '=');
    index += 3;
  }
  return ss;
}
std::vector<uint8_t> base64ToBytes(const std::string& b64_in) {
    // If it might be base64url, normalize first:
    std::string b64 = b64_in;
    for (auto& c : b64) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    // pad to multiple of 4
    while (b64.size() % 4) b64.push_back('=');

    size_t out_len = 0;
    // First call to get required length
    mbedtls_base64_decode(nullptr, 0, &out_len,
        reinterpret_cast<const unsigned char*>(b64.data()), b64.size());

    std::vector<uint8_t> out(out_len);
    int rc = mbedtls_base64_decode(out.data(), out.size(), &out_len,
        reinterpret_cast<const unsigned char*>(b64.data()), b64.size());
    if (rc != 0) return {};   // handle error as you like
    out.resize(out_len);
    return out;
}
std::vector<uint8_t> stringHexToBytes(const std::string& s) {
  std::vector<uint8_t> v;
  v.reserve(s.length() / 2);

  for (std::string::size_type i = 0; i < s.length(); i += 2) {
    std::string byteString = s.substr(i, 2);
    uint8_t byte = (uint8_t)strtol(byteString.c_str(), NULL, 16);
    v.push_back(byte);
  }

  return v;
}

std::string bytesToHexString(const std::vector<uint8_t>& v) {
  std::stringstream ss;
  ss << std::hex << std::setfill('0');
  std::vector<uint8_t>::const_iterator it;

  for (it = v.begin(); it != v.end(); it++) {
    ss << std::setw(2) << static_cast<unsigned>(*it);
  }

  return ss.str();
}

std::vector<uint8_t> bigNumAdd(std::vector<uint8_t> num, int n) {
  auto carry = n;
  for (int x = num.size() - 1; x >= 0; x--) {
    int res = num[x] + carry;
    if (res < 256) {
      carry = 0;
      num[x] = res;
    } else {
      // Carry the rest of the division
      carry = res / 256;
      num[x] = res % 256;

      // extend the vector at the last index
      if (x == 0) {
        num.insert(num.begin(), carry);
        return num;
      }
    }
  }

  return num;
}

std::vector<uint8_t> bigNumDivide(std::vector<uint8_t> num, int n) {
  auto carry = 0;
  for (int x = 0; x < num.size(); x++) {
    int res = num[x] + carry * 256;
    if (res < n) {
      carry = res;
      num[x] = 0;
    } else {
      // Carry the rest of the division
      carry = res % n;
      num[x] = res / n;
    }
  }

  return num;
}

std::vector<uint8_t> bigNumMultiply(std::vector<uint8_t> num, int n) {
  auto carry = 0;
  for (int x = num.size() - 1; x >= 0; x--) {
    int res = num[x] * n + carry;
    if (res < 256) {
      carry = 0;
      num[x] = res;
    } else {
      // Carry the rest of the division
      carry = res / 256;
      num[x] = res % 256;

      // extend the vector at the last index
      if (x == 0) {
        num.insert(num.begin(), carry);
        return num;
      }
    }
  }

  return num;
}
unsigned char h2int(char c) {
  if (c >= '0' && c <= '9') {
    return ((unsigned char)c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return ((unsigned char)c - 'a' + 10);
  }
  if (c >= 'A' && c <= 'F') {
    return ((unsigned char)c - 'A' + 10);
  }
  return (0);
}

std::string urlDecode(std::string str) {
  std::string encodedString = "";
  char c;
  char code0;
  char code1;
  for (int i = 0; i < str.length(); i++) {
    c = str[i];
    if (c == '+') {
      encodedString += ' ';
    } else if (c == '%') {
      i++;
      code0 = str[i];
      i++;
      code1 = str[i];
      c = (h2int(code0) << 4) | h2int(code1);
      encodedString += c;
    } else {

      encodedString += c;
    }
  }

  return encodedString;
}

std::pair<SpotifyFileType, std::vector<uint8_t>> base62Decode(std::string uri) {
  std::vector<uint8_t> n = std::vector<uint8_t>({0});
  SpotifyFileType type = SpotifyFileType::UNKNOWN;
  auto it = uri.begin();
  if (uri.find(":") != std::string::npos) {
    if (uri.find("episode:") != std::string::npos) {
      type = SpotifyFileType::EPISODE;
    } else if (uri.find("track:") != std::string::npos) {
      type = SpotifyFileType::TRACK;
    }
    it += uri.rfind(":") + 1;
  }
  while (it != uri.end()) {
    size_t d = Base62Alphabet.find(*it);
    n = bigNumMultiply(n, 62);
    n = bigNumAdd(n, d);
    it++;
  }

  return std::make_pair(type, n);
}

static inline const char* toTypeString(SpotifyFileType t) {
    switch (t) {
        case SpotifyFileType::TRACK:   return "track";
        case SpotifyFileType::EPISODE: return "episode";
        default:                       return "unknown";
    }
}

// Teilt einen big-endian Bytevektor durch 62.
// Gibt Quotient (ebenfalls big-endian, ohne führende Nullen) und Rest zurück.
static std::pair<std::vector<uint8_t>, uint8_t>
divmod62(const std::vector<uint8_t>& be) {
    std::vector<uint8_t> q; q.reserve(be.size());
    uint32_t rem = 0;
    for (uint8_t b : be) {
        uint32_t cur = (rem << 8) | b;
        uint8_t qb = static_cast<uint8_t>(cur / 62);
        rem = static_cast<uint8_t>(cur % 62);
        if (!q.empty() || qb != 0) q.push_back(qb);
    }
    return {q, static_cast<uint8_t>(rem)};
}

static inline std::vector<uint8_t> stripLeadingZeros(std::vector<uint8_t> v) {
    auto it = std::find_if(v.begin(), v.end(), [](uint8_t x){ return x != 0; });
    v.erase(v.begin(), it);
    return v;
}

// Bytes (big-endian) → Base62-String
std::string base62FromBytes(const std::vector<uint8_t>& bytesBE) {
    std::vector<uint8_t> v = bytesBE;             // no stripping
    if (v.empty()) return "0";
    std::string out;
    while (!v.empty()) {
        auto [q, r] = divmod62(v);
        out.push_back(Base62Alphabet[r]);
        v = std::move(q);
    }
    std::reverse(out.begin(), out.end());
    return out;
}
static inline std::string padTo22(std::string s) {
    if (s.empty()) return {};               // treat empty as invalid
    if (s.size() < 22) s.insert(0, 22 - s.size(), '0');
    return s;
}
// (Type, Bytes) → "spotify:<type>:<base62>"
std::string base62EncodeUri(const std::pair<SpotifyFileType, std::vector<uint8_t>>& in) {
    printf("Encoding ");
    for(uint8_t b : in.second) {
        printf("%i ", b);
    }
    printf("\n");
    const char* typeStr = toTypeString(in.first);
    std::string b62 = base62FromBytes(in.second);
    b62 = padTo22(b62);                        // <- canonicalize to 22
    printf("Encoded %s\n", b62.c_str());
    return std::string("spotify:") + typeStr + ":" + b62;
}