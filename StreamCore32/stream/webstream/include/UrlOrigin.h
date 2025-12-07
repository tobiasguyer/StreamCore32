#pragma once
#include <algorithm>
#include <string>
#include <vector>

namespace streamcore::helpers {

inline std::string schemeOf(const std::string& origin) {
  auto p = origin.find("://");
  return (p == std::string::npos) ? std::string("https") : origin.substr(0, p);
}
inline std::string hostOf(const std::string& origin) {
  auto p = origin.find("://");
  size_t start = (p == std::string::npos) ? 0 : p + 3;
  size_t slash = origin.find('/', start);
  return origin.substr(
      start, (slash == std::string::npos) ? std::string::npos : slash - start);
}

static std::vector<std::string> genOriginVariants(const std::string& origin) {
  std::vector<std::string> out;
  auto sch = schemeOf(origin);
  auto h = hostOf(origin);

  auto add = [&](const std::string& host) {
    std::string u = sch + "://" + host;
    if (std::find(out.begin(), out.end(), u) == out.end())
      out.push_back(std::move(u));
  };

  // 1) original
  add(h);

  // 2) reduced host: first label + "." + TLD (e.g., jam-on.ch from jam-on.ice.infomaniak.ch)
  size_t firstDot = h.find('.');
  size_t lastDot = h.rfind('.');
  if (firstDot != std::string::npos && lastDot != std::string::npos &&
      firstDot < lastDot) {
    std::string first = h.substr(0, firstDot);
    std::string tld = h.substr(lastDot + 1);
    if (!first.empty() && !tld.empty())
      add(first + "." + tld);
  }

  // 3) apex = last two labels (e.g., infomaniak.ch), plus www.apex
  if (lastDot != std::string::npos) {
    size_t pre = h.rfind('.', lastDot - 1);
    if (pre != std::string::npos) {
      std::string apex = h.substr(pre + 1);
      add(apex);
      add("www." + apex);
    }
  }

  return out;
}

}  // namespace streamcore::helpers