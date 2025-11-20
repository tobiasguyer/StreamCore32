#pragma once
#include <string>
#include <map>
#include <vector>
#include <optional>
#include <memory>
#include <BellTask.h>
#include <BellLogger.h>
#include <WrappedSemaphore.h>

class QobuzConfig : public bell::Task {
public:
  // What we try to extract
  struct AppKey{ std::string id, secret; };
struct Seed { std::string tz_cap; std::string seed; };

  struct ClientAppSecrets {
    std::string id;
    std::map<std::string, std::string> seeds;
    std::map<std::string, std::string> secrets ={};
    std::vector<std::string> api_keys;
    std::optional<std::string> token;
    // For anything extra the site exposes
    std::map<std::string, std::string> extras;

    bool empty() const {
      return !secrets.size() ||  !api_keys.size();
    }
  };

  // Tuning knobs
  struct SecretScrapeOptions {
    std::string url = "https://play.qobuz.com/login";
    // If the player page references a JSON bootstrap/config URL, we’ll follow it.
    bool follow_config_links = true;
    // Max redirects across requests.
    int max_redirects = 4;
    // Timeout in milliseconds for each request (connect + IO).
    int timeout_ms = 15000;
    // Extra headers, if you need a special UA or cookies.
    std::vector<std::pair<std::string, std::string>> extra_headers;
  };

  QobuzConfig(ClientAppSecrets* secrets, const SecretScrapeOptions* opts = nullptr)
                : bell::Task("QobuzConfig", 48 * 1024, 2, 1) {
    loadedSemaphore = std::make_shared<bell::WrappedSemaphore>(1, 0);
    secrets_ = secrets;
    if(opts) opts_ = *opts;
    startTask();
  }

  ~QobuzConfig() {
    loadedSemaphore->give();
  }
  void runTask() override{
    *secrets_ = FetchClientAppSecrets();
    loadedSemaphore->give();
  }
  /// Fetch the page at `player_url` via Bell HTTP/TLS, then:
  /// 1) If it looks like JSON, parse and extract well-known keys.
  /// 2) If it’s HTML/JS, scrape common inline JSON config blobs.
  /// 3) If we find a config URL in the page (e.g., window.__CONFIG__, data-config, etc.)
  ///    we follow it (when follow_config_links=true) and extract secrets from there.
  /// Returns a filled ClientAppSecrets or std::nullopt if nothing found.
  /// Throws std::runtime_error on hard network/parse errors.
  ClientAppSecrets FetchClientAppSecrets();

  std::shared_ptr<bell::WrappedSemaphore> loadedSemaphore;
  // Replace your findQuotedString(...) with this version (no <regex>):
  private:
  ClientAppSecrets* secrets_;
  SecretScrapeOptions opts_;
};