// NvsCreds.cpp — NVS creds with per-call worker task (esp_err_t-based)
// Each public API spawns a short-lived internal RAM task that performs
// the NVS operation and then self-deletes. The caller waits using a
// task notification (ulTaskNotifyTake).

#include "NvsCreds.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>


extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_system.h"


#include "esp_check.h"
#include "esp_log.h"


#include "nvs.h"
#include "nvs_flash.h"

#include "cJSON.h"

#include "mbedtls/base64.h"
#include "mbedtls/gcm.h"
#include "mbedtls/sha256.h"

}
#include "SecureKeyHelper.h"

// ===============================
// Configuration for the worker
// ===============================
#ifndef NVS_WORKER_TASK_STACK
#define NVS_WORKER_TASK_STACK (4096)  // stack size in words (4096 * 4 bytes)
#endif

#ifndef NVS_WORKER_TASK_PRIO
#define NVS_WORKER_TASK_PRIO (configMAX_PRIORITIES - 2)
#endif

#ifndef NVS_WORKER_TASK_CORE
// Pin to core 0 by default (many IDF examples do NVS on core 0).
#define NVS_WORKER_TASK_CORE (0)
#endif

static std::atomic<bool> g_nvs_initialized = false;
static std::vector<TaskHandle_t> g_nvs_init_waiter = {};

// ===============================
// Internal helpers (non-RTOS specific)
// ===============================
namespace {

constexpr const char* kCurrKey = "current";
constexpr const char* kPartName = "nvs";
constexpr const char* kNamePref = "n_";
constexpr const char* kBlobPref = "b_";

// Encrypted blob layout: 4 bytes magic + 12 bytes IV + CIPHERTEXT + 16 bytes TAG
static const uint8_t kMagic[4] = {'N', 'V', 'S', 'C'};
constexpr size_t kGcmIvLen = 12;
constexpr size_t kGcmTagLen = 16;

inline void strip_nvs_cstr(std::string& s,
                           size_t nvs_reported_len_including_nul) {
  if (nvs_reported_len_including_nul > 0 &&
      nvs_reported_len_including_nul <= s.size())
    s.resize(nvs_reported_len_including_nul - 1);
}

inline std::string to_lower(std::string s) {
  for (auto& c : s)
    c = (char)std::tolower((unsigned char)c);
  return s;
}

inline std::vector<uint8_t> as_bytes(const std::string& s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}
inline std::string as_string(const std::vector<uint8_t>& v) {
  return std::string(v.begin(), v.end());
}

// base64 helpers
inline std::string b64encode(const std::vector<uint8_t>& in) {
  size_t olen = 0;
  (void)mbedtls_base64_encode(nullptr, 0, &olen, in.data(), in.size());
  std::string out;
  out.resize(olen);
  if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(&out[0]),
                            out.size(), &olen, in.data(), in.size()) != 0)
    return {};
  out.resize(olen);
  return out;
}
inline std::vector<uint8_t> b64decode(const std::string& in) {
  size_t olen = 0;
  (void)mbedtls_base64_decode(nullptr, 0, &olen,
                              reinterpret_cast<const unsigned char*>(in.data()),
                              in.size());
  std::vector<uint8_t> out;
  out.resize(olen);
  if (mbedtls_base64_decode(out.data(), out.size(), &olen,
                            reinterpret_cast<const unsigned char*>(in.data()),
                            in.size()) != 0)
    return {};
  out.resize(olen);
  return out;
}

// key suffix (<= 13 chars payload after 2-char prefix -> 15 total)
// Use SHA-256(userkey) -> first 13 hex chars for compact key names.
inline std::string key_suffix_for(const std::string& userkey) {
  uint8_t hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  mbedtls_sha256_update_ret(
      &ctx, reinterpret_cast<const uint8_t*>(userkey.data()), userkey.size());
  mbedtls_sha256_finish_ret(&ctx, hash);
  mbedtls_sha256_free(&ctx);

  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(13);
  for (size_t i = 0; out.size() < 13; ++i) {
    out.push_back(hex[(hash[i] >> 4) & 0xF]);
    if (out.size() == 13)
      break;
    out.push_back(hex[hash[i] & 0xF]);
  }
  return out;
}

inline std::string make_name_key(const std::string& suffix) {
  return std::string(kNamePref) + suffix;
}
inline std::string make_blob_key(const std::string& suffix) {
  return std::string(kBlobPref) + suffix;
}

inline bool blob_is_encrypted(const uint8_t* blob, size_t len) {
  return len >= 4 && std::memcmp(blob, kMagic, 4) == 0;
}

// AES-GCM helpers (no AAD)
inline esp_err_t gcm_encrypt(const std::vector<uint8_t>& key32,
                             const std::string& plaintext,
                             std::vector<uint8_t>& out_blob) {
  if (key32.size() != 32)
    return ESP_ERR_INVALID_ARG;

  // blob = MAGIC(4) | IV(12) | CIPHERTEXT | TAG(16)
  uint8_t iv[kGcmIvLen];
  for (size_t i = 0; i < kGcmIvLen; ++i)
    iv[i] = (uint8_t)esp_random();

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key32.data(), 256) != 0) {
    mbedtls_gcm_free(&gcm);
    return ESP_FAIL;
  }

  std::vector<uint8_t> ct(plaintext.size());
  uint8_t tag[kGcmTagLen];

  if (mbedtls_gcm_crypt_and_tag(
          &gcm, MBEDTLS_GCM_ENCRYPT, plaintext.size(), iv, sizeof(iv), nullptr,
          0, reinterpret_cast<const uint8_t*>(plaintext.data()), ct.data(),
          sizeof(tag), tag) != 0) {
    mbedtls_gcm_free(&gcm);
    return ESP_FAIL;
  }
  mbedtls_gcm_free(&gcm);

  out_blob.clear();
  out_blob.insert(out_blob.end(), kMagic, kMagic + 4);
  out_blob.insert(out_blob.end(), iv, iv + sizeof(iv));
  out_blob.insert(out_blob.end(), ct.begin(), ct.end());
  out_blob.insert(out_blob.end(), tag, tag + sizeof(tag));
  return ESP_OK;
}

inline esp_err_t gcm_decrypt(const std::vector<uint8_t>& key32,
                             const uint8_t* blob, size_t len,
                             std::string& out_plain) {
  if (key32.size() != 32)
    return ESP_ERR_INVALID_ARG;
  if (!blob_is_encrypted(blob, len))
    return ESP_ERR_INVALID_ARG;
  if (len < 4 + kGcmIvLen + kGcmTagLen)
    return ESP_ERR_INVALID_SIZE;

  const uint8_t* iv = blob + 4;
  const uint8_t* data = blob + 4 + kGcmIvLen;
  size_t data_len = len - 4 - kGcmIvLen - kGcmTagLen;
  const uint8_t* tag = blob + len - kGcmTagLen;

  std::vector<uint8_t> pt(data_len);

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key32.data(), 256) != 0) {
    mbedtls_gcm_free(&gcm);
    return ESP_FAIL;
  }

  int rc = mbedtls_gcm_auth_decrypt(&gcm, data_len, iv, kGcmIvLen, nullptr, 0,
                                    tag, kGcmTagLen, data, pt.data());
  mbedtls_gcm_free(&gcm);
  if (rc != 0)
    return ESP_ERR_INVALID_RESPONSE;

  out_plain.assign(reinterpret_cast<const char*>(pt.data()), pt.size());
  return ESP_OK;
}

// JSON <-> fields helpers
inline cJSON* fields_to_json(const std::vector<Field>& fields) {
  cJSON* obj = cJSON_CreateObject();
  for (const auto& f : fields) {
    std::string b64 = b64encode(f.value);
    cJSON_AddStringToObject(obj, f.name.c_str(), b64.c_str());
  }
  return obj;
}
inline std::vector<Field> json_to_fields(cJSON* obj /* can be nullptr */) {
  std::vector<Field> out;
  if (!obj)
    return out;
  cJSON* it = nullptr;
  cJSON_ArrayForEach(it, obj) {
    if (cJSON_IsString(it) && it->string) {
      Field f;
      f.name = it->string;
      f.value = b64decode(it->valuestring ? it->valuestring : "");
      out.push_back(std::move(f));
    }
  }
  return out;
}

// Locate the suffix for a userkey by scanning "n_*" entries.
inline esp_err_t find_suffix_for_userkey(nvs_handle_t h, const char* ns,
                                         const std::string& userkey,
                                         std::string& out_suf) {
  nvs_iterator_t it = nullptr;
  esp_err_t err = nvs_entry_find(kPartName, ns, NVS_TYPE_STR, &it);
  while (err == ESP_OK && it != nullptr) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    err = nvs_entry_next(&it);
    if (std::strncmp(info.key, kNamePref, 2) != 0)
      continue;

    size_t len = 0;
    if (nvs_get_str(h, info.key, nullptr, &len) != ESP_OK)
      continue;
    std::string u(len, '\0');
    if (nvs_get_str(h, info.key, &u[0], &len) != ESP_OK)
      continue;
    strip_nvs_cstr(u, len);
    if (u == userkey) {
      out_suf = std::string(info.key + 2);
      return ESP_OK;
    }
  }
  return ESP_ERR_NOT_FOUND;
}

}  // namespace

// ===============================
// Tiny per-call task runner
// ===============================

namespace {

// Context for a single NVS worker task invocation.
struct NvsTaskCtx {
  // Function to run inside worker (must perform the whole NVS op).
  esp_err_t (*fn)(void* arg);
  void* arg;

  // Who to notify on completion.
  TaskHandle_t waiter;

  // Result written by worker before notifying.
  esp_err_t result;
};

// The worker executes the function, notifies the waiter, and self-deletes.
static void nvs_worker_task(void* pv) {
  // Wait until NVS is initialized
  if (!g_nvs_initialized.load()) {
    TaskHandle_t waiter = xTaskGetCurrentTaskHandle();
    g_nvs_init_waiter.push_back(waiter);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // Wait for notification
    waiter = nullptr;
  }
  NvsTaskCtx* ctx = static_cast<NvsTaskCtx*>(pv);
  ctx->result = ctx->fn ? ctx->fn(ctx->arg) : ESP_ERR_INVALID_ARG;
  printf("SecureStore::nvs_worker_task() => %s\n",
         esp_err_to_name(ctx->result));
  // wake up the waiter
  xTaskNotifyGive(ctx->waiter);

  // self-destroy
  vTaskDelete(nullptr);
}

// Runs `fn(arg)` in a fresh worker task pinned to internal RAM, waits, returns result.
inline esp_err_t run_in_nvs_task(esp_err_t (*fn)(void*), void* arg) {

  NvsTaskCtx* ctx = static_cast<NvsTaskCtx*>(heap_caps_malloc(
      sizeof(NvsTaskCtx), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!ctx)
    return ESP_ERR_NO_MEM;
  ctx->fn = fn;
  ctx->arg = arg;
  ctx->waiter = xTaskGetCurrentTaskHandle();
  ctx->result = ESP_FAIL;

  TaskHandle_t th = nullptr;
  BaseType_t ok = xTaskCreatePinnedToCore(
      nvs_worker_task, "nvsw", NVS_WORKER_TASK_STACK, ctx, NVS_WORKER_TASK_PRIO,
      &th, NVS_WORKER_TASK_CORE);
  if (ok != pdPASS) {
    heap_caps_free(ctx);
    return ESP_ERR_NO_MEM;
  }

  // Wait for completion (no timeout).
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  esp_err_t result = ctx->result;

  // Free the ctx (allocated in internal RAM)
  heap_caps_free(ctx);
  return result;  // ctx was freed in worker after notify, but result copied above
}

}  // namespace

// ===============================
// Global key for SecureStore
// ===============================
static std::vector<uint8_t> g_master_key(32);  // 32 bytes
static bool g_has_key = false;

esp_err_t init_nvs(const uint8_t* key, size_t len) {
  // NVS init is safe in caller, but keep it here for symmetry and simplicity.
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  if (err == ESP_OK) {
    g_nvs_initialized.store(true);
    // Set up the master key
    char msg[32];
    if (!key || len != 32)
      err = sc32_get_master_key(&g_master_key[0]);
    else
      g_master_key.assign(key, key + len);
    g_has_key = true;
    // Notify any waiters
    if (g_nvs_init_waiter.size()) {
      for (int i = 0; i < g_nvs_init_waiter.size(); ++i) {
        xTaskNotifyGive(g_nvs_init_waiter[i]);
        g_nvs_init_waiter[i] = nullptr;
      }
      g_nvs_init_waiter.clear();
    }
  }
  return err;
}

Store::Store(const char* ns) : ns_(ns) {}

// ---- Worker bodies (static) and thin wrappers ----

struct SaveArgs {
  const char* ns;
  Record r;
  bool overwrite;
};
static esp_err_t w_store_save(void* pv) {
  auto* a = static_cast<SaveArgs*>(pv);
  const std::string suf = key_suffix_for(a->r.userkey);
  const std::string kname = make_name_key(suf);
  const std::string kblob = make_blob_key(suf);

  nvs_handle_t h{};
  esp_err_t e = nvs_open(a->ns, NVS_READWRITE, &h);
  if (e != ESP_OK)
    return e;

  if (!a->overwrite) {
    size_t len = 0;
    if (nvs_get_str(h, kname.c_str(), nullptr, &len) == ESP_OK) {
      nvs_close(h);
      return ESP_ERR_INVALID_STATE;
    }
  }

  cJSON* root = fields_to_json(a->r.fields);
  char* js = cJSON_PrintUnformatted(root);
  if (!js) {
    cJSON_Delete(root);
    nvs_close(h);
    return ESP_ERR_NO_MEM;
  }

  e = nvs_set_str(h, kname.c_str(), a->r.userkey.c_str());
  if (e == ESP_OK)
    e = nvs_set_blob(h, kblob.c_str(), js, std::strlen(js));
  cJSON_free(js);
  cJSON_Delete(root);
  if (e == ESP_OK)
    e = nvs_commit(h);
  nvs_close(h);
  return e;
}
esp_err_t Store::save(const Record& r, bool overwrite) {
  SaveArgs a{ns_, r, overwrite};
  return run_in_nvs_task(w_store_save, &a);
}

struct LoadArgs {
  const char* ns;
  std::string userkey;
  Record* out;
};
static esp_err_t w_store_load(void* pv) {
  auto* a = static_cast<LoadArgs*>(pv);
  if (!a->out)
    return ESP_ERR_INVALID_ARG;

  std::string suf = key_suffix_for(a->userkey);
  std::string kname = make_name_key(suf);

  nvs_handle_t h{};
  esp_err_t e = nvs_open(a->ns, NVS_READONLY, &h);
  if (e != ESP_OK)
    return e;

  size_t l = 0;
  e = nvs_get_str(h, kname.c_str(), nullptr, &l);
  if (e != ESP_OK) {
    std::string suf2;
    if (find_suffix_for_userkey(h, a->ns, a->userkey, suf2) != ESP_OK) {
      nvs_close(h);
      return ESP_ERR_NOT_FOUND;
    }
    suf = std::move(suf2);
    kname = make_name_key(suf);
  }

  const std::string kblob = make_blob_key(suf);

  size_t bl = 0;
  if (nvs_get_blob(h, kblob.c_str(), nullptr, &bl) != ESP_OK) {
    nvs_close(h);
    return ESP_ERR_NOT_FOUND;
  }
  std::string js;
  js.resize(bl);
  if (nvs_get_blob(h, kblob.c_str(), &js[0], &bl) != ESP_OK) {
    nvs_close(h);
    return ESP_FAIL;
  }

  cJSON* root = cJSON_ParseWithLength(js.data(), js.size());
  if (!root) {
    nvs_close(h);
    return ESP_ERR_INVALID_RESPONSE;
  }
  a->out->userkey = a->userkey;
  a->out->fields = json_to_fields(root);
  cJSON_Delete(root);
  nvs_close(h);
  return ESP_OK;
}
esp_err_t Store::load(const std::string& userkey, Record* out) {
  LoadArgs a{ns_, userkey, out};
  return run_in_nvs_task(w_store_load, &a);
}

struct ListArgs {
  const char* ns;
  std::vector<std::string>* out;
};
static esp_err_t w_store_list(void* pv) {
  auto* a = static_cast<ListArgs*>(pv);
  if (!a->out)
    return ESP_ERR_INVALID_ARG;
  a->out->clear();

  nvs_handle_t h{};
  esp_err_t e = nvs_open(a->ns, NVS_READONLY, &h);
  if (e != ESP_OK)
    return e;
  nvs_iterator_t it = nullptr;
  esp_err_t err = nvs_entry_find(kPartName, a->ns, NVS_TYPE_STR, &it);
  while (err == ESP_OK && it != nullptr) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    err = nvs_entry_next(&it);
    if (std::strncmp(info.key, kNamePref, 2) != 0)
      continue;

    size_t len = 0;
    if (nvs_get_str(h, info.key, nullptr, &len) != ESP_OK)
      continue;
    std::string u(len, '\0');
    if (nvs_get_str(h, info.key, &u[0], &len) != ESP_OK)
      continue;
    strip_nvs_cstr(u, len);
    a->out->push_back(std::move(u));
  }
  nvs_close(h);
  return ESP_OK;
}
esp_err_t Store::list(std::vector<std::string>& out_userkeys) {
  ListArgs a{ns_, &out_userkeys};
  return run_in_nvs_task(w_store_list, &a);
}

struct SearchArgs {
  const char* ns;
  std::string q;
  bool ci;
  bool pref;
  std::vector<std::string>* out;
};
static esp_err_t w_store_search(void* pv) {
  auto* a = static_cast<SearchArgs*>(pv);
  if (!a->out)
    return ESP_ERR_INVALID_ARG;
  a->out->clear();
  std::string q = a->ci ? to_lower(a->q) : a->q;

  nvs_handle_t h{};
  esp_err_t e = nvs_open(a->ns, NVS_READONLY, &h);
  if (e != ESP_OK)
    return e;
  nvs_iterator_t it = nullptr;
  esp_err_t err = nvs_entry_find(kPartName, a->ns, NVS_TYPE_STR, &it);
  while (err == ESP_OK && it != nullptr) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    err = nvs_entry_next(&it);
    if (std::strncmp(info.key, kNamePref, 2) != 0)
      continue;

    size_t len = 0;
    if (nvs_get_str(h, info.key, nullptr, &len) != ESP_OK)
      continue;
    std::string u(len, '\0');
    if (nvs_get_str(h, info.key, &u[0], &len) != ESP_OK)
      continue;
    strip_nvs_cstr(u, len);

    std::string cmp = a->ci ? to_lower(u) : u;
    bool match = a->pref ? (cmp.rfind(q, 0) == 0)
                         : (q.empty() || cmp.find(q) != std::string::npos);
    if (match)
      a->out->push_back(std::move(u));
  }
  nvs_close(h);
  return ESP_OK;
}
esp_err_t Store::search_userkeys(const std::string& query,
                                 std::vector<std::string>& out_userkeys,
                                 bool case_insensitive, bool prefix_only) {
  SearchArgs a{ns_, query, case_insensitive, prefix_only, &out_userkeys};
  return run_in_nvs_task(w_store_search, &a);
}

struct ExistsArgs {
  const char* ns;
  std::string userkey;
  esp_err_t* out;
};
static esp_err_t w_store_exists(void* pv) {
  auto* a = static_cast<ExistsArgs*>(pv);
  nvs_handle_t h{};
  esp_err_t e = nvs_open(a->ns, NVS_READONLY, &h);
  if (e != ESP_OK)
    return e;

  const std::string kname = make_name_key(key_suffix_for(a->userkey));
  size_t l = 0;
  if (nvs_get_str(h, kname.c_str(), nullptr, &l) == ESP_OK) {
    std::string u(l, '\0');
    if (nvs_get_str(h, kname.c_str(), &u[0], &l) == ESP_OK) {
      strip_nvs_cstr(u, l);
      nvs_close(h);
      *a->out = (u == a->userkey) ? ESP_OK : ESP_ERR_NOT_FOUND;
      return ESP_OK;
    }
  }
  std::string suf;
  e = find_suffix_for_userkey(h, a->ns, a->userkey, suf);
  nvs_close(h);
  *a->out = (e == ESP_OK) ? ESP_OK : ESP_ERR_NOT_FOUND;
  return ESP_OK;
}
esp_err_t Store::exists(const std::string& userkey) {
  esp_err_t present = ESP_ERR_NOT_FOUND;
  ExistsArgs a{ns_, userkey, &present};
  esp_err_t run = run_in_nvs_task(w_store_exists, &a);
  return (run == ESP_OK) ? present : run;
}

struct EraseArgs {
  const char* ns;
  std::string userkey;
};
static esp_err_t w_store_erase(void* pv) {
  auto* a = static_cast<EraseArgs*>(pv);
  nvs_handle_t h{};
  esp_err_t e = nvs_open(a->ns, NVS_READWRITE, &h);
  if (e != ESP_OK)
    return e;

  std::string suf;
  if (find_suffix_for_userkey(h, a->ns, a->userkey, suf) != ESP_OK) {
    nvs_close(h);
    return ESP_ERR_NOT_FOUND;
  }

  esp_err_t e1 = nvs_erase_key(h, make_name_key(suf).c_str());
  esp_err_t e2 = nvs_erase_key(h, make_blob_key(suf).c_str());

  // Clear "current" if it points to this record
  size_t l = 0;
  if (nvs_get_str(h, kCurrKey, nullptr, &l) == ESP_OK) {
    std::string curr(l, '\0');
    if (nvs_get_str(h, kCurrKey, &curr[0], &l) == ESP_OK) {
      strip_nvs_cstr(curr, l);
      if (curr == a->userkey)
        (void)nvs_erase_key(h, kCurrKey);
    }
  }
  e = (e1 == ESP_OK || e2 == ESP_OK) ? nvs_commit(h)
                                     : ((e1 != ESP_OK) ? e1 : e2);
  nvs_close(h);
  return e;
}
esp_err_t Store::erase(const std::string& userkey) {
  EraseArgs a{ns_, userkey};
  return run_in_nvs_task(w_store_erase, &a);
}

struct SetCurArgs {
  const char* ns;
  std::string userkey;
};
static esp_err_t w_store_setcur(void* pv) {
  auto* a = static_cast<SetCurArgs*>(pv);
  nvs_handle_t h{};
  esp_err_t e = nvs_open(a->ns, NVS_READWRITE, &h);
  if (e != ESP_OK)
    return e;
  e = nvs_set_str(h, kCurrKey, a->userkey.c_str());
  if (e == ESP_OK)
    e = nvs_commit(h);
  nvs_close(h);
  return e;
}
esp_err_t Store::set_current(const std::string& userkey) {
  SetCurArgs a{ns_, userkey};
  return run_in_nvs_task(w_store_setcur, &a);
}

struct GetCurArgs {
  const char* ns;
  Record* out;
};
static esp_err_t w_store_getcur(void* pv) {
  auto* a = static_cast<GetCurArgs*>(pv);
  if (!a->out)
    return ESP_ERR_INVALID_ARG;

  nvs_handle_t h{};
  esp_err_t e = nvs_open(a->ns, NVS_READONLY, &h);
  if (e != ESP_OK)
    return e;

  size_t l = 0;
  e = nvs_get_str(h, kCurrKey, nullptr, &l);
  if (e != ESP_OK) {
    nvs_close(h);
    return e;
  }
  std::string uk(l, '\0');
  e = nvs_get_str(h, kCurrKey, &uk[0], &l);
  nvs_close(h);
  if (e != ESP_OK)
    return e;
  strip_nvs_cstr(uk, l);

  // Reuse load in worker too
  LoadArgs la{a->ns, uk, a->out};
  return w_store_load(&la);
}
esp_err_t Store::get_current(Record* out) {
  GetCurArgs a{ns_, out};
  return run_in_nvs_task(w_store_getcur, &a);
}

struct UpdFieldArgs {
  const char* ns;
  std::string userkey, fname;
  std::vector<uint8_t> val;
  bool create;
};
static esp_err_t w_store_update_field(void* pv) {
  auto* a = static_cast<UpdFieldArgs*>(pv);
  if (a->fname.empty())
    return ESP_ERR_INVALID_ARG;

  // load (or create empty)
  Record r;
  LoadArgs la{a->ns, a->userkey, &r};
  esp_err_t e = w_store_load(&la);
  if (e == ESP_ERR_NOT_FOUND && a->create) {
    r.userkey = a->userkey;
    r.fields.clear();
  } else if (e != ESP_OK) {
    return e;
  }

  auto it = std::find_if(r.fields.begin(), r.fields.end(),
                         [&](const Field& f) { return f.name == a->fname; });
  if (it == r.fields.end())
    r.fields.push_back(Field{a->fname, a->val});
  else
    it->value = a->val;

  SaveArgs sa{a->ns, r, true};
  return w_store_save(&sa);
}
esp_err_t Store::update_field(const std::string& userkey,
                              const std::string& field_name,
                              const std::vector<uint8_t>& value,
                              bool create_record_if_missing) {
  UpdFieldArgs a{ns_, userkey, field_name, value, create_record_if_missing};
  return run_in_nvs_task(w_store_update_field, &a);
}

struct GetFieldArgs {
  const char* ns;
  std::string userkey, fname;
  std::vector<uint8_t>* out;
};
static esp_err_t w_store_get_field(void* pv) {
  auto* a = static_cast<GetFieldArgs*>(pv);
  if (!a->out)
    return ESP_ERR_INVALID_ARG;
  Record r;
  LoadArgs la{a->ns, a->userkey, &r};
  esp_err_t e = w_store_load(&la);
  if (e != ESP_OK)
    return e;
  for (auto& f : r.fields)
    if (f.name == a->fname) {
      *a->out = f.value;
      return ESP_OK;
    }
  return ESP_ERR_NOT_FOUND;
}
esp_err_t Store::get_field(const std::string& userkey,
                           const std::string& field_name,
                           std::vector<uint8_t>* out) {
  GetFieldArgs a{ns_, userkey, field_name, out};
  return run_in_nvs_task(w_store_get_field, &a);
}

esp_err_t Store::set_field_str(const std::string& userkey,
                               const std::string& field_name,
                               const std::string& value,
                               bool create_record_if_missing) {
  return update_field(userkey, field_name, as_bytes(value),
                      create_record_if_missing);
}

esp_err_t Store::get_field_str(const std::string& userkey,
                               const std::string& field_name,
                               std::string* out) {
  if (!out)
    return ESP_ERR_INVALID_ARG;
  std::vector<uint8_t> v;
  esp_err_t e = get_field(userkey, field_name, &v);
  if (e != ESP_OK)
    return e;
  *out = as_string(v);
  return ESP_OK;
}

struct ExportArgs {
  const char* ns;
  const char* path;
};
static esp_err_t w_store_export(void* pv) {
  auto* a = static_cast<ExportArgs*>(pv);
  if (!a->path)
    return ESP_ERR_INVALID_ARG;

  std::vector<std::string> users;
  ListArgs la{a->ns, &users};
  ESP_RETURN_ON_ERROR(w_store_list(&la), "NvsCreds", "list failed");

  cJSON* root = cJSON_CreateArray();
  for (auto& u : users) {
    Record r;
    LoadArgs l2{a->ns, u, &r};
    if (w_store_load(&l2) != ESP_OK)
      continue;
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "userkey", r.userkey.c_str());
    cJSON_AddItemToObject(o, "fields", fields_to_json(r.fields));
    cJSON_AddItemToArray(root, o);
  }
  char* js = cJSON_Print(root);
  if (!js) {
    cJSON_Delete(root);
    return ESP_ERR_NO_MEM;
  }

  FILE* f = std::fopen(a->path, "w");
  if (!f) {
    cJSON_free(js);
    cJSON_Delete(root);
    return ESP_FAIL;
  }
  std::fwrite(js, 1, std::strlen(js), f);
  std::fclose(f);
  cJSON_free(js);
  cJSON_Delete(root);
  return ESP_OK;
}
esp_err_t Store::export_to_spiffs(const char* path) {
  ExportArgs a{ns_, path};
  return run_in_nvs_task(w_store_export, &a);
}

struct ImportArgs {
  const char* ns;
  const char* path;
  bool overwrite;
};
static esp_err_t w_store_import(void* pv) {
  auto* a = static_cast<ImportArgs*>(pv);
  if (!a->path)
    return ESP_ERR_INVALID_ARG;
  FILE* f = std::fopen(a->path, "r");
  if (!f)
    return ESP_FAIL;

  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::string buf;
  buf.resize(n);
  (void)std::fread(&buf[0], 1, n, f);
  std::fclose(f);

  cJSON* root = cJSON_ParseWithLength(buf.data(), buf.size());
  if (!root)
    return ESP_ERR_INVALID_RESPONSE;

  auto handle_obj = [&](cJSON* o) {
    cJSON* uk = cJSON_GetObjectItem(o, "userkey");
    cJSON* fs = cJSON_GetObjectItem(o, "fields");
    if (cJSON_IsString(uk) && (cJSON_IsObject(fs) || fs == nullptr)) {
      Record r;
      r.userkey = uk->valuestring ? uk->valuestring : "";
      r.fields = json_to_fields(fs ? fs : cJSON_CreateObject());
      if (!r.userkey.empty()) {
        SaveArgs sa{a->ns, r, a->overwrite};
        (void)w_store_save(&sa);
      }
    }
  };

  if (cJSON_IsArray(root)) {
    const int sz = cJSON_GetArraySize(root);
    for (int i = 0; i < sz; ++i) {
      cJSON* o = cJSON_GetArrayItem(root, i);
      if (cJSON_IsObject(o))
        handle_obj(o);
    }
  } else if (cJSON_IsObject(root)) {
    handle_obj(root);
  }
  cJSON_Delete(root);
  return ESP_OK;
}
esp_err_t Store::import_from_spiffs(const char* path, bool overwrite) {
  ImportArgs a{ns_, path, overwrite};
  return run_in_nvs_task(w_store_import, &a);
}

struct FirstArgs {
  const char* ns;
  Record* out;
};
static esp_err_t w_store_get_first(void* pv) {
  auto* a = static_cast<FirstArgs*>(pv);
  if (!a->out)
    return ESP_ERR_INVALID_ARG;

  nvs_handle_t h{};
  esp_err_t e = nvs_open(a->ns, NVS_READONLY, &h);
  if (e != ESP_OK)
    return e;

  std::string best_suf;
  nvs_iterator_t it = nullptr;
  esp_err_t err = nvs_entry_find(kPartName, a->ns, NVS_TYPE_STR, &it);
  while (err == ESP_OK && it != nullptr) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    err = nvs_entry_next(&it);
    if (std::strncmp(info.key, kNamePref, 2) == 0) {
      std::string suf(info.key + 2);
      if (best_suf.empty() || make_name_key(suf) < make_name_key(best_suf))
        best_suf = std::move(suf);
    }
  }

  if (best_suf.empty()) {
    nvs_close(h);
    return ESP_ERR_NOT_FOUND;
  }

  const std::string kname = make_name_key(best_suf);
  const std::string kblob = make_blob_key(best_suf);

  size_t len = 0;
  if (nvs_get_str(h, kname.c_str(), nullptr, &len) != ESP_OK) {
    nvs_close(h);
    return ESP_ERR_NOT_FOUND;
  }
  std::string u(len, '\0');
  if (nvs_get_str(h, kname.c_str(), &u[0], &len) != ESP_OK) {
    nvs_close(h);
    return ESP_FAIL;
  }
  strip_nvs_cstr(u, len);

  size_t bl = 0;
  if (nvs_get_blob(h, kblob.c_str(), nullptr, &bl) != ESP_OK) {
    nvs_close(h);
    return ESP_ERR_NOT_FOUND;
  }
  std::string js(bl, '\0');
  if (nvs_get_blob(h, kblob.c_str(), &js[0], &bl) != ESP_OK) {
    nvs_close(h);
    return ESP_FAIL;
  }

  cJSON* root = cJSON_ParseWithLength(js.data(), js.size());
  if (!root) {
    nvs_close(h);
    return ESP_ERR_INVALID_RESPONSE;
  }
  a->out->userkey = std::move(u);
  a->out->fields = json_to_fields(root);
  cJSON_Delete(root);
  nvs_close(h);
  return ESP_OK;
}
esp_err_t Store::get_first(Record* out) {
  FirstArgs a{ns_, out};
  return run_in_nvs_task(w_store_get_first, &a);
}

struct StartupArgs {
  const char* ns;
  bool setcur;
  Record* out;
};
static esp_err_t w_store_get_startup(void* pv) {
  auto* a = static_cast<StartupArgs*>(pv);
  if (!a->out)
    return ESP_ERR_INVALID_ARG;

  // Try current
  GetCurArgs ga{a->ns, a->out};
  if (w_store_getcur(&ga) == ESP_OK)
    return ESP_OK;

  // Otherwise first and optionally set current
  FirstArgs fa{a->ns, a->out};
  if (w_store_get_first(&fa) == ESP_OK) {
    if (a->setcur) {
      SetCurArgs sa{a->ns, a->out->userkey};
      (void)w_store_setcur(&sa);
    }
    return ESP_OK;
  }
  return ESP_ERR_NOT_FOUND;
}
esp_err_t Store::get_startup_record(Record* out, bool set_as_current) {
  StartupArgs a{ns_, set_as_current, out};
  return run_in_nvs_task(w_store_get_startup, &a);
}

// ===============================
// SecureStore (AES-GCM encrypted)
// ===============================

SecureStore::SecureStore(const char* ns) : ns_(ns) {}

// Secure worker bodies and wrappers (mirror Store but encrypt blobs)

struct SSaveArgs {
  const char* ns;
  Record r;
  bool overwrite;
};
static esp_err_t w_secure_save(void* pv) {
  auto* a = static_cast<SSaveArgs*>(pv);
  if (!g_has_key || g_master_key.size() != 32)
    return ESP_ERR_INVALID_STATE;

  const std::string suf = key_suffix_for(a->r.userkey);
  const std::string kname = make_name_key(suf);
  const std::string kblob = make_blob_key(suf);

  nvs_handle_t h{};
  esp_err_t e = nvs_open(a->ns, NVS_READWRITE, &h);
  if (e != ESP_OK)
    return e;

  if (!a->overwrite) {
    size_t len = 0;
    if (nvs_get_str(h, kname.c_str(), nullptr, &len) == ESP_OK) {
      nvs_close(h);
      return ESP_ERR_INVALID_STATE;
    }
  }

  cJSON* root = fields_to_json(a->r.fields);
  char* js = cJSON_PrintUnformatted(root);
  if (!js) {
    cJSON_Delete(root);
    nvs_close(h);
    return ESP_ERR_NO_MEM;
  }

  std::vector<uint8_t> payload;
  e = gcm_encrypt(g_master_key, std::string(js, js + std::strlen(js)), payload);
  cJSON_free(js);
  cJSON_Delete(root);
  if (e != ESP_OK) {
    nvs_close(h);
    return e;
  }

  e = nvs_set_str(h, kname.c_str(), a->r.userkey.c_str());
  if (e == ESP_OK)
    e = nvs_set_blob(h, kblob.c_str(), payload.data(), payload.size());
  if (e == ESP_OK)
    e = nvs_commit(h);
  nvs_close(h);
  return e;
}
int SecureStore::save(const Record& r, bool overwrite) {
  SSaveArgs a{ns_, r, overwrite};
  printf("SecureStore::save(%s)\n", r.userkey.c_str());
  return run_in_nvs_task(w_secure_save, &a);
}

struct SLoadArgs {
  const char* ns;
  std::string userkey;
  Record* out;
};
static esp_err_t w_secure_load(void* pv) {
  auto* a = static_cast<SLoadArgs*>(pv);
  if (!a->out)
    return ESP_ERR_INVALID_ARG;
  if (!g_has_key || g_master_key.size() != 32)
    return ESP_ERR_INVALID_STATE;

  std::string suf = key_suffix_for(a->userkey);
  std::string kname = make_name_key(suf);

  nvs_handle_t h{};
  esp_err_t e = nvs_open(a->ns, NVS_READONLY, &h);
  if (e != ESP_OK)
    printf("SecureStore::w_secure_load() => %s\n", esp_err_to_name(e));
  if (e != ESP_OK)
    return e;

  size_t l = 0;
  e = nvs_get_str(h, kname.c_str(), nullptr, &l);
  if (e != ESP_OK) {
    std::string suf2;
    if (find_suffix_for_userkey(h, a->ns, a->userkey, suf2) != ESP_OK) {
      nvs_close(h);
      return ESP_ERR_NOT_FOUND;
    }
    suf = std::move(suf2);
    kname = make_name_key(suf);
  }
  if (e != ESP_OK)
    printf("SecureStore::w_secure_load() => %s\n", esp_err_to_name(e));

  const std::string kblob = make_blob_key(suf);
  size_t bl = 0;
  if (nvs_get_blob(h, kblob.c_str(), nullptr, &bl) != ESP_OK) {
    nvs_close(h);
    return ESP_ERR_NOT_FOUND;
  }
  std::vector<uint8_t> blob(bl);
  if (nvs_get_blob(h, kblob.c_str(), blob.data(), &bl) != ESP_OK) {
    nvs_close(h);
    return ESP_FAIL;
  }
  nvs_close(h);

  if (e != ESP_OK)
    printf("SecureStore::w_secure_load() => %s\n", esp_err_to_name(e));
  if (!blob_is_encrypted(blob.data(), blob.size()))
    return ESP_ERR_INVALID_RESPONSE;

  std::string js;
  e = gcm_decrypt(g_master_key, blob.data(), blob.size(), js);
  if (e != ESP_OK)
    printf("SecureStore::w_secure_load() => %s\n", esp_err_to_name(e));
  if (e != ESP_OK)
    return e;

  cJSON* root = cJSON_ParseWithLength(js.data(), js.size());
  if (!root)
    return ESP_ERR_INVALID_RESPONSE;
  a->out->userkey = a->userkey;
  a->out->fields = json_to_fields(root);
  cJSON_Delete(root);
  e = ESP_OK;
  printf("userkey=%s\n,SecureStore::w_secure_load() => %s\n",
         a->out->userkey.c_str(), esp_err_to_name(e));
  return e;
}
int SecureStore::load(const std::string& userkey, Record* out) {
  SLoadArgs a{ns_, userkey, out};
  return run_in_nvs_task(w_secure_load, &a);
}

struct SListArgs {
  const char* ns;
  std::vector<std::string>* out;
};
static esp_err_t w_secure_list(void* pv) {
  // identical to Store list()
  return w_store_list(pv);
}
int SecureStore::list(std::vector<std::string>& out_userkeys) {
  SListArgs a{ns_, &out_userkeys};
  return run_in_nvs_task(w_secure_list, &a);
}

struct SSrchArgs {
  const char* ns;
  std::string q;
  bool ci;
  bool pref;
  std::vector<std::string>* out;
};
static esp_err_t w_secure_search(void* pv) {
  return w_store_search(pv);
}
esp_err_t SecureStore::search_userkeys(const std::string& query,
                                       std::vector<std::string>& out_userkeys,
                                       bool case_insensitive,
                                       bool prefix_only) {
  SSrchArgs a{ns_, query, case_insensitive, prefix_only, &out_userkeys};
  return run_in_nvs_task(w_secure_search, &a);
}

struct SExistsArgs {
  const char* ns;
  std::string userkey;
  esp_err_t* out;
};
static esp_err_t w_secure_exists(void* pv) {
  return w_store_exists(pv);
}
esp_err_t SecureStore::exists(const std::string& userkey) {
  esp_err_t present = ESP_ERR_NOT_FOUND;
  SExistsArgs a{ns_, userkey, &present};
  esp_err_t run = run_in_nvs_task(w_secure_exists, &a);
  return (run == ESP_OK) ? present : run;
}

struct SEraseArgs {
  const char* ns;
  std::string userkey;
};
static esp_err_t w_secure_erase(void* pv) {
  return w_store_erase(pv);
}
esp_err_t SecureStore::erase(const std::string& userkey) {
  SEraseArgs a{ns_, userkey};
  return run_in_nvs_task(w_secure_erase, &a);
}

struct SSetCurArgs {
  const char* ns;
  std::string userkey;
};
static esp_err_t w_secure_setcur(void* pv) {
  return w_store_setcur(pv);
}
int SecureStore::set_current(const std::string& userkey) {
  SSetCurArgs a{ns_, userkey};
  printf("SecureStore::set_current(%s)\n", userkey.c_str());
  return run_in_nvs_task(w_secure_setcur, &a);
}

struct SGetCurArgs {
  const char* ns;
  Record* out;
};
static esp_err_t w_secure_getcur(void* pv) {
  auto* a = static_cast<SGetCurArgs*>(pv);
  if (!a->out)
    return ESP_ERR_INVALID_ARG;
  printf("SecureStore::w_secure_get_current()\n");
  // reuse Store::get_current worker then decrypt via load
  nvs_handle_t h{};
  esp_err_t e = nvs_open(a->ns, NVS_READONLY, &h);
  if (e != ESP_OK)
    printf("SecureStore::w_secure_get_current() => %s\n", esp_err_to_name(e));
  if (e != ESP_OK)
    return e;

  size_t l = 0;
  e = nvs_get_str(h, kCurrKey, nullptr, &l);
  if (e != ESP_OK) {
    nvs_close(h);
    printf("SecureStore::w_secure_get_current() => %s\n", esp_err_to_name(e));
    return e;
  }
  std::string uk(l, '\0');
  e = nvs_get_str(h, kCurrKey, &uk[0], &l);
  nvs_close(h);
  printf("SecureStore::w_secure_get_current() => %s\n", esp_err_to_name(e));
  if (e != ESP_OK)
    return e;
  strip_nvs_cstr(uk, l);

  SLoadArgs la{a->ns, uk, a->out};
  return w_secure_load(&la);
}
int SecureStore::get_current(Record* out) {
  SGetCurArgs a{ns_, out};
  printf("SecureStore::get_current()\n");
  esp_err_t err = run_in_nvs_task(w_secure_getcur, &a);
  printf("SecureStore::get_current() => %s\n",
         out ? out->userkey.c_str() : "(null)");
  if (err != ESP_OK)
    printf("err=%i\n", err);
  return err;
}

struct SUpdFieldArgs {
  const char* ns;
  std::string userkey, fname;
  std::vector<uint8_t> val;
  bool create;
};
static esp_err_t w_secure_update_field(void* pv) {
  auto* a = static_cast<SUpdFieldArgs*>(pv);
  if (a->fname.empty())
    return ESP_ERR_INVALID_ARG;

  Record r;
  SLoadArgs la{a->ns, a->userkey, &r};
  esp_err_t e = w_secure_load(&la);
  if (e == ESP_ERR_NOT_FOUND && a->create) {
    r.userkey = a->userkey;
    r.fields.clear();
  } else if (e != ESP_OK) {
    return e;
  }

  auto it = std::find_if(r.fields.begin(), r.fields.end(),
                         [&](const Field& f) { return f.name == a->fname; });
  if (it == r.fields.end())
    r.fields.push_back(Field{a->fname, a->val});
  else
    it->value = a->val;

  SSaveArgs sa{a->ns, r, true};
  return w_secure_save(&sa);
}
esp_err_t SecureStore::update_field(const std::string& userkey,
                                    const std::string& field_name,
                                    const std::vector<uint8_t>& value,
                                    bool create_record_if_missing) {
  SUpdFieldArgs a{ns_, userkey, field_name, value, create_record_if_missing};
  return run_in_nvs_task(w_secure_update_field, &a);
}

struct SGetFieldArgs {
  const char* ns;
  std::string userkey, fname;
  std::vector<uint8_t>* out;
};
static esp_err_t w_secure_get_field(void* pv) {
  auto* a = static_cast<SGetFieldArgs*>(pv);
  if (!a->out)
    return ESP_ERR_INVALID_ARG;
  Record r;
  SLoadArgs la{a->ns, a->userkey, &r};
  esp_err_t e = w_secure_load(&la);
  if (e != ESP_OK)
    return e;
  for (auto& f : r.fields)
    if (f.name == a->fname) {
      *a->out = f.value;
      return ESP_OK;
    }
  return ESP_ERR_NOT_FOUND;
}
esp_err_t SecureStore::get_field(const std::string& userkey,
                                 const std::string& field_name,
                                 std::vector<uint8_t>* out) {
  SGetFieldArgs a{ns_, userkey, field_name, out};
  return run_in_nvs_task(w_secure_get_field, &a);
}

esp_err_t SecureStore::set_field_str(const std::string& userkey,
                                     const std::string& field_name,
                                     const std::string& value,
                                     bool create_record_if_missing) {
  return update_field(userkey, field_name, as_bytes(value),
                      create_record_if_missing);
}

esp_err_t SecureStore::get_field_str(const std::string& userkey,
                                     const std::string& field_name,
                                     std::string* out) {
  if (!out)
    return ESP_ERR_INVALID_ARG;
  std::vector<uint8_t> v;
  esp_err_t e = get_field(userkey, field_name, &v);
  if (e != ESP_OK)
    return e;
  *out = as_string(v);
  return ESP_OK;
}

struct SExportArgs {
  const char* ns;
  const char* path;
};
static esp_err_t w_secure_export(void* pv) {
  // Export decrypted JSON (same as Store)
  return w_store_export(pv);
}
esp_err_t SecureStore::export_to_spiffs(const char* path) {
  SExportArgs a{ns_, path};
  return run_in_nvs_task(w_secure_export, &a);
}

struct SImportArgs {
  const char* ns;
  const char* path;
  bool overwrite;
};
static esp_err_t w_secure_import(void* pv) {
  // Import plaintext JSON and store encrypted
  auto* a = static_cast<SImportArgs*>(pv);
  if (!a->path)
    return ESP_ERR_INVALID_ARG;
  FILE* f = std::fopen(a->path, "r");
  if (!f)
    return ESP_FAIL;

  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::string buf;
  buf.resize(n);
  (void)std::fread(&buf[0], 1, n, f);
  std::fclose(f);

  cJSON* root = cJSON_ParseWithLength(buf.data(), buf.size());
  if (!root)
    return ESP_ERR_INVALID_RESPONSE;

  auto handle_obj = [&](cJSON* o) {
    cJSON* uk = cJSON_GetObjectItem(o, "userkey");
    cJSON* fs = cJSON_GetObjectItem(o, "fields");
    if (cJSON_IsString(uk) && (cJSON_IsObject(fs) || fs == nullptr)) {
      Record r;
      r.userkey = uk->valuestring ? uk->valuestring : "";
      r.fields = json_to_fields(fs ? fs : cJSON_CreateObject());
      if (!r.userkey.empty()) {
        SSaveArgs sa{a->ns, r, a->overwrite};
        (void)w_secure_save(&sa);
      }
    }
  };

  if (cJSON_IsArray(root)) {
    const int sz = cJSON_GetArraySize(root);
    for (int i = 0; i < sz; ++i) {
      cJSON* o = cJSON_GetArrayItem(root, i);
      if (cJSON_IsObject(o))
        handle_obj(o);
    }
  } else if (cJSON_IsObject(root)) {
    handle_obj(root);
  }
  cJSON_Delete(root);
  return ESP_OK;
}
esp_err_t SecureStore::import_from_spiffs(const char* path, bool overwrite) {
  SImportArgs a{ns_, path, overwrite};
  return run_in_nvs_task(w_secure_import, &a);
}

struct SFirstArgs {
  const char* ns;
  Record* out;
};
static esp_err_t w_secure_get_first(void* pv) {
  // Reuse Store::get_first worker, then decrypt via normal load path
  auto* a = static_cast<SFirstArgs*>(pv);
  if (!a->out)
    return ESP_ERR_INVALID_ARG;

  // get the first userkey (plaintext name is stored in n_*)
  nvs_handle_t h{};
  esp_err_t e = nvs_open(a->ns, NVS_READONLY, &h);
  if (e != ESP_OK)
    return e;
  std::string best_suf;
  nvs_iterator_t it = nullptr;
  esp_err_t err = nvs_entry_find(kPartName, a->ns, NVS_TYPE_STR, &it);
  while (err == ESP_OK && it != nullptr) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    err = nvs_entry_next(&it);
    if (std::strncmp(info.key, kNamePref, 2) == 0) {
      std::string suf(info.key + 2);
      if (best_suf.empty() || make_name_key(suf) < make_name_key(best_suf))
        best_suf = std::move(suf);
    }
  }
  if (best_suf.empty()) {
    nvs_close(h);
    return ESP_ERR_NOT_FOUND;
  }
  size_t len = 0;
  std::string kname = make_name_key(best_suf);
  if (nvs_get_str(h, kname.c_str(), nullptr, &len) != ESP_OK) {
    nvs_close(h);
    return ESP_ERR_NOT_FOUND;
  }
  std::string uk(len, '\0');
  if (nvs_get_str(h, kname.c_str(), &uk[0], &len) != ESP_OK) {
    nvs_close(h);
    return ESP_FAIL;
  }
  strip_nvs_cstr(uk, len);
  nvs_close(h);

  SLoadArgs la{a->ns, uk, a->out};
  return w_secure_load(&la);
}
esp_err_t SecureStore::get_first(Record* out) {
  SFirstArgs a{ns_, out};
  return run_in_nvs_task(w_secure_get_first, &a);
}

struct SStartupArgs {
  const char* ns;
  bool setcur;
  Record* out;
};
static esp_err_t w_secure_get_startup(void* pv) {
  auto* a = static_cast<SStartupArgs*>(pv);
  if (!a->out)
    return ESP_ERR_INVALID_ARG;

  SGetCurArgs ga{a->ns, a->out};
  if (w_secure_getcur(&ga) == ESP_OK)
    return ESP_OK;

  SFirstArgs fa{a->ns, a->out};
  if (w_secure_get_first(&fa) == ESP_OK) {
    if (a->setcur) {
      SSetCurArgs sa{a->ns, a->out->userkey};
      (void)w_secure_setcur(&sa);
    }
    return ESP_OK;
  }
  return ESP_ERR_NOT_FOUND;
}
esp_err_t SecureStore::get_startup_record(Record* out, bool set_as_current) {
  SStartupArgs a{ns_, set_as_current, out};
  return run_in_nvs_task(w_secure_get_startup, &a);
}
