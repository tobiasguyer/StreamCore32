#pragma once
#include <string>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <atomic>
#include <optional>
#include <nlohmann/json.hpp>

class LeaseRegistry {
public:
  struct LeaseRec {
    uint64_t id;
    std::string resource;            // e.g. "net.http.civet" or "net.mdns"
    nlohmann::json owner;            // free-form meta: {"component":"spotify","port":7864}
    std::chrono::steady_clock::time_point acquired_at;
  };

  struct ResourceBucket {
    size_t count = 0;
    std::function<void()> on_first;  // called when count goes 0 -> 1
    std::function<void()> on_last;   // called when count goes 1 -> 0
  };

  static LeaseRegistry& instance() {
    static LeaseRegistry inst; return inst;
  }

  // Acquire a lease, returning a unique ID.
  uint64_t acquire(const std::string& resource,
                   const nlohmann::json& owner,
                   std::function<void()> on_first = {},
                   std::function<void()> on_last = {}) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& b = buckets_[resource];
    if (b.count == 0) {
      if (!b.on_first) b.on_first = std::move(on_first);
      if (!b.on_last)  b.on_last  = std::move(on_last);
      if (b.on_first)  b.on_first();           // start resource
    }
    b.count++;

    const uint64_t id = ++next_id_;
    LeaseRec rec{ id, resource, owner, std::chrono::steady_clock::now() };
    leases_.emplace(id, rec);
    order_.push_back(id);
    return id;
  }

  void release(uint64_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = leases_.find(id);
    if (it == leases_.end()) return;
    const std::string resource = it->second.resource;
    leases_.erase(it);
    auto& b = buckets_[resource];
    if (b.count > 0 && --b.count == 0) {
      if (b.on_last) b.on_last();             // stop resource
    }
  }

  nlohmann::json snapshot() {
    std::lock_guard<std::mutex> lk(mu_);
    using nlohmann::json;
    json j;
    json jr = json::array();
    for (auto& [name, b] : buckets_) {
      jr.push_back({{"resource",name},{"count",b.count}});
    }
    json jl = json::array();
    for (auto id : order_) {
      auto it = leases_.find(id);
      if (it == leases_.end()) continue;
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - it->second.acquired_at).count();
      jl.push_back({
        {"id", it->second.id},
        {"resource", it->second.resource},
        {"owner", it->second.owner},
        {"age_ms", ms}
      });
    }
    j["resources"] = jr;
    j["leases"] = jl;
    return j;
  }

  // RAII guard
  class Guard {
  public:
    Guard() = default;
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    Guard(Guard&& o) noexcept { id_ = o.id_; o.id_ = 0; }
    Guard& operator=(Guard&& o) noexcept {
      if (this != &o) { reset(); id_ = o.id_; o.id_ = 0; }
      return *this;
    }
    ~Guard(){ reset(); }

    static Guard acquire(const std::string& resource,
                         const nlohmann::json& owner,
                         std::function<void()> on_first = {},
                         std::function<void()> on_last = {}) {
      Guard g;
      g.id_ = LeaseRegistry::instance().acquire(resource, owner, std::move(on_first), std::move(on_last));
      return g;
    }
    void reset() {
      if (id_) { LeaseRegistry::instance().release(id_); id_ = 0; }
    }
    explicit operator bool() const { return id_ != 0; }
  private:
    uint64_t id_ = 0;
  };

private:
  LeaseRegistry() = default;
  std::mutex mu_;
  std::unordered_map<std::string, ResourceBucket> buckets_;
  std::unordered_map<uint64_t, LeaseRec> leases_;
  std::vector<uint64_t> order_;
  std::atomic<uint64_t> next_id_{0};
};
