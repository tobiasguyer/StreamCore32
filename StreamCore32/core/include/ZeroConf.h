#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "BellHTTPServer.h"
#include "BellLogger.h"
#include "MDNSService.h"
#include "nlohmann/json.hpp"

// civetweb forward decls (pulled in by BellHTTPServer)
struct mg_connection;
struct mg_request_info;
struct mg_header;

// ---------- tiny helpers available to modules ----------
inline std::string zc_makeHexId(size_t n = 16) {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  static const char* k = "0123456789abcdef";
  std::string s;
  s.resize(n);
  for (size_t i = 0; i < n; ++i)
    s[i] = k[rng() & 0xF];
  return s;
}

// Parses JSON or x-www-form-urlencoded request body into a map<string,string>.
inline std::map<std::string, std::string> zc_parseBodyToMap(
    mg_connection* conn) {
  std::map<std::string, std::string> out;
  std::string body;
  if (const auto* ri = mg_get_request_info(conn)) {
    if (ri->content_length > 0) {
      body.resize(ri->content_length);
      mg_read(conn, body.data(), ri->content_length);

      // Try JSON first
      auto first = body.find_first_not_of(" \t\r\n");
      if (first != std::string::npos && body[first] == '{') {
        try {
          auto j = nlohmann::json::parse(body);
          for (auto it = j.begin(); it != j.end(); ++it) {
            if (it->is_string())
              out[it.key()] = it->get<std::string>();
            else
              out[it.key()] = it->dump();
          }
          return out;
        } catch (...) { /* fall through */
        }
      }
      // Fallback: x-www-form-urlencoded
      mg_header hdrs[64];
      int num = mg_split_form_urlencoded(body.data(), hdrs, 64);
      for (int i = 0; i < num; ++i)
        out[hdrs[i].name] = hdrs[i].value;
    }
  }
  return out;
}

// ======================= ZeroconfServiceManager =======================
class ZeroconfServiceManager {
 public:
  ZeroconfServiceManager() = default;

  bool initialize(const std::string& deviceName, int port = 12345) {
    if (initialized_)
      return true;
    deviceName_ = deviceName;
    serverPort_ = port;

    std::vector<std::pair<std::string, std::string>> opts = {
        {"thread_stack_size", "12288"},
        {"num_threads", "5"},
        {"prespawn_threads", "3"},
        {"connection_queue", "5"},
    };
    server_ = std::make_unique<bell::BellHTTPServer>(serverPort_, opts);

    server_->registerGet("/close", [this](mg_connection*) {
      BELL_LOG(info, "zeroconf", "Closing connection via /close");
      if (onClose_)
        onClose_();
      return server_->makeEmptyResponse();
    });

    initialized_ = true;
    return true;
  }

  bool isInitialized() const { return initialized_; }
  const std::string& deviceName() const { return deviceName_; }
  int serverPort() const { return serverPort_; }

  // ---------- Generic endpoint + service spec ----------
  enum class HttpMethod { GET, POST };

  struct Endpoint {
    HttpMethod method;
    std::string path;  // e.g., "/spotify_info" or "/qobuz/display_info"
    std::function<std::string(mg_connection*)>
        handler;  // returns a JSON string body
  };

  struct ServiceSpec {
    std::string key;          // unique id (e.g., "spotify", "qobuz")
    std::string serviceType;  // e.g., "_spotify-connect", "_qobuz-connect"
    std::string proto = "_tcp";
    std::string instanceName;  // default: deviceName()
    std::map<std::string, std::string>
        txt;  // e.g., {"VERSION","1.0"}, {"CPath","/qobuz/display_info"}
    std::vector<Endpoint> endpoints;  // endpoints to register on THIS server
  };

  // Registers HTTP endpoints and announces mDNS. No app-specific dependencies.
  bool addService(const ServiceSpec& spec) {
    if (!initialized_) {
      BELL_LOG(error, "zeroconf", "ZC manager not initialized");
      return false;
    }
    if (spec.key.empty() || spec.serviceType.empty()) {
      BELL_LOG(error, "zeroconf",
               "addService: key and serviceType are required");
      return false;
    }
    if (services_.count(spec.key)) {
      BELL_LOG(error, "zeroconf", "addService: key '{}' already exists",
               spec.key.c_str());
      return false;
    }

    ServiceRecord rec;
    // Register endpoints
    for (const auto& ep : spec.endpoints) {
      if (ep.path.empty() || !ep.handler) {
        BELL_LOG(error, "zeroconf", "addService[%s]: invalid endpoint",
                 spec.key.c_str());
        return false;
      }
      if (ep.method == HttpMethod::GET) {
        server_->registerGet(ep.path, [this, h = ep.handler](mg_connection* c) {
          return server_->makeJsonResponse(h(c));
        });
      } else {  // POST
        server_->registerPost(ep.path,
                              [this, h = ep.handler](mg_connection* c) {
                                return server_->makeJsonResponse(h(c));
                              });
      }
      if (std::find(rec.endpoints.begin(), rec.endpoints.end(), ep.path) ==
          rec.endpoints.end())
        rec.endpoints.push_back(ep.path);
    }

    // Build TXT (defaults)
    std::map<std::string, std::string> txt = spec.txt;

    // mDNS announce
    rec.key = spec.key;
    rec.instanceName =
        spec.instanceName.empty() ? deviceName_ : spec.instanceName;
    rec.serviceType = spec.serviceType;
    rec.proto = spec.proto;
    rec.port = serverPort_;
    rec.txtRecords = std::move(txt);

    rec.mdns = bell::MDNSService::registerService(rec.instanceName,
                                                  rec.serviceType, rec.proto,
                                                  "", rec.port, rec.txtRecords);

    services_.emplace(rec.key, std::move(rec));
    return true;
  }

  bool removeService(const std::string& key) {
    auto it = services_.find(key);
    if (it == services_.end())
      return false;
    if (it->second.endpoints.size() > 0) {
      for (const auto& ep : it->second.endpoints) {
        server_->unregisterEndpoint(ep);
      }
    }
    if (it->second.mdns) {
      it->second.mdns->unregisterService();
      it->second.mdns.reset();
    }
    services_.erase(it);
    return true;
  }

  bool hasService(const std::string& key) const {
    return services_.count(key) > 0;
  }

  // Optional: expose a close hook
  std::function<void()> onClose_;

 private:
  struct ServiceRecord {
    std::string key;
    std::string instanceName;
    std::string serviceType;
    std::string proto = "_tcp";
    int port = 0;
    std::map<std::string, std::string> txtRecords;
    std::vector<std::string> endpoints;
    std::unique_ptr<bell::MDNSService> mdns;
  };

  bool initialized_ = false;
  int serverPort_ = 12345;
  std::string deviceName_;

  std::unique_ptr<bell::BellHTTPServer> server_;
  std::unordered_map<std::string, ServiceRecord> services_;
};

// --------- Global instance and init shim ----------
extern ZeroconfServiceManager zeroconf;
bool InitZeroconf(const std::string& deviceName, int port = 12345);
