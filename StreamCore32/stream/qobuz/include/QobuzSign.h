#pragma once
#include <string>
#include <vector>
#include <utility>

namespace qobuz {

  // Build request_sig for Qobuz private API (MD5, lowercase hex).
  // - object: "session", "track", "file", "user", ...
  // - method: "start", "getFileUrl", "url", "login", ...
  // - params: ONLY body/query params (NOT headers), e.g. {{"profile","qbz-1"}}
  // - ts_text: the exact text you send as request_ts (prefer float seconds via timesync::now_s_text())
  // - app_secret: production app secret (already “unpacked” if needed)
  std::string md5_sig(const std::string& object,
    const std::string& method,
    const std::vector<std::pair<std::string, std::string>>& params,
    const std::string& ts_text,
    const std::string& app_secret);

  // Utility: build the urlencoded query "k=v&k2=v2" from params (no ts/sig)
  std::string build_query(const std::vector<std::pair<std::string, std::string>>& params);

  // (Optional hook) If your secret is stored packed, transform here. Default: no-op.
  std::string maybe_unpack_secret(const std::string& secret);

} // namespace qbz
