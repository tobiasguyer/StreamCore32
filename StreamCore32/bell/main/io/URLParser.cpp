#include "URLParser.h"

namespace bell {

#ifdef BELL_DISABLE_REGEX
static bool starts_with(const char* s, const char* p) {
  while (*p && *s && *p == *s) { ++p; ++s; }
  return *p == '\0';
}

void URLParser::parse(const char* url, std::vector<std::string>& match) {
  match.assign(6, std::string()); // [0] full, [1] scheme, [2] host[:port], [3] path, [4] ?query, [5] #hash
  if (!url || !*url) { match.clear(); return; }
  match[0] = url;

  const char* p = url;

  // scheme
  const char* colon = strchr(p, ':');
  if (colon) {
    match[1].assign(p, colon - p);
    p = colon + 1;
  }

  // "//" authority
  if (starts_with(p, "//")) {
    p += 2;
    // authority runs until '/', '?', or '#'
    const char* auth_end = p;
    while (*auth_end && *auth_end != '/' && *auth_end != '?' && *auth_end != '#') ++auth_end;
    if (auth_end == p) { match.clear(); return; }
    match[2].assign(p, auth_end - p);
    p = auth_end;
  }

  // path
  if (*p == '/') {
    const char* path_end = p;
    while (*path_end && *path_end != '?' && *path_end != '#') ++path_end;
    match[3].assign(p, path_end - p);
    p = path_end;
  } else {
    match[3] = "/"; // RFC: empty path ⇒ "/"
  }

  // query
  if (*p == '?') {
    const char* q = ++p;
    while (*p && *p != '#') ++p;
    match[4] = "?";
    match[4].append(q, p - q);
  }

  // fragment
  if (*p == '#') {
    ++p;
    match[5] = "#";
    match[5].append(p);
  }

  // minimal validity
  if (match[1].empty()) match[1] = "http";
  if (match[2].empty()) { match.clear(); return; }
}
#else
const std::regex URLParser::urlParseRegex = std::regex(
    "^(?:([^:/?#]+):)?(?://([^/?#]*))?([^?#]*)(\\?(?:[^#]*))?(#(?:.*))?");
#endif
}  // namespace bell
