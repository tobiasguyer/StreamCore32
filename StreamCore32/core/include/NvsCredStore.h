#pragma once

// NvsCredStore.h — ESP-IDF 4.x compatible, generic multi-source credential store
// - Pure C NVS API usage (no C++ wrappers required by user app)
// - Store multiple credentials ("records"), each identified by a user key (clientid/userkey)
// - Each record holds arbitrary named fields (any source, any variables), stored as a single blob
//   containing a JSON object of { fieldName: base64(value) } to keep NVS keys short & safe
// - Manage: init, save, load, exists, list, search, erase, set/get current, update field, get field
// - Optional export/import to SPIFFS as JSON (one or array), compatible with the in-blob format
// - Careful to respect NVS 15-char key limit by hashing the user key to a short suffix
//
// Example usage:
//   #include "NvsCredStore.h"
//   ESP_ERROR_CHECK(nvsc::init_nvs());
//   nvsc::Record r{"alice", {{"access_token", bytesAT}, {"refresh_token", bytesRT}}};
//   ESP_ERROR_CHECK(nvsc::save(r, /*overwrite=*/true));
//   nvsc::Record any;if (nvsc::get_startup_record(&any)==ESP_OK) { /* use credentials */ }
//
// Notes:
//  * Field values are binary-safe (std::vector<uint8_t>). Convenience helpers for string fields are provided.
//  * The store uses two NVS keys per record: name key (n_<suffix>) and blob key (b_<suffix>).
//  * A global key "current" holds the active user key. Namespace is configurable at compile time.
//
// Dependencies: ESP-IDF 4.4.x (nvs, nvs_flash, esp_err, esp_log), mbedTLS (sha256, base64), cJSON

#include <cctype>
#include <map>
#include <string>
#include <vector>

#include "BellLogger.h"
#include "SecureKeyHelper.h"  // sc32_get_master_key(out32)
#include "cJSON.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* DEFAULT_NS = "cred";  // NVS namespace (customize if needed)
static std::vector<uint8_t> master_key_(32, 0);
namespace nvsc {

static const char* TAG = "nvsc";
static const char* DEFAULT_NS = "cred";
// NVS namespace (customize if needed)
static const char* CURR_KEY = "current";
// NVS key for current user key

struct Field {
  std::string name;
  // field name (e.g., "access_token", "cert", "auth_blob")
  std::vector<uint8_t> value;
  // binary-safe payload
};

struct Record {
  std::string userkey;
  // primary identifier for a credential set (clientid/userkey)
  std::vector<Field> fields;
  // arbitrary named fields
};

// ----------------- helpers -------------------------------------------------
inline std::string to_hex(const uint8_t* b, size_t n) {
  static const char* h = "0123456789abcdef";
  std::string s;
  s.resize(n * 2);
  for (size_t i = 0; i < n; ++i) {
    s[2 * i] = h[b[i] >> 4];
    s[2 * i + 1] = h[b[i] & 0xF];
  }
  return s;
}

// 12 hex chars (6 bytes) → safe & short (fits n_/b_ + 12 under 15-char key limit)
inline std::string key_suffix_for(const std::string& userkey) {
  uint8_t hash[32];
  mbedtls_sha256_ret(reinterpret_cast<const unsigned char*>(userkey.data()),
                     userkey.size(), hash, 0);
  return to_hex(hash, 6);
}
inline std::string key_user(const std::string& suffix) {
  return std::string("n_") + suffix;
}
inline std::string key_blob(const std::string& suffix) {
  return std::string("b_") + suffix;
}

inline std::string to_lower(std::string s) {
  for (auto& c : s)
    c = (char)std::tolower((unsigned char)c);
  return s;
}
inline void strip_nvs_cstr(std::string& s, size_t nvs_len_including_nul) {
  if (nvs_len_including_nul > 0 && nvs_len_including_nul <= s.size())
    s.resize(nvs_len_including_nul - 1);
}

// base64 helpers
inline std::string b64encode(const std::vector<uint8_t>& in) {
  size_t olen = 0;
  mbedtls_base64_encode(nullptr, 0, &olen, in.data(), in.size());
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
  mbedtls_base64_decode(nullptr, 0, &olen,
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
inline bool blob_is_encrypted(const uint8_t* blob, size_t len) {
  return len >= 4 && memcmp(blob, kMagic, 4) == 0;
}

inline esp_err_t gcm_encrypt(const std::vector<uint8_t>& key32,
                             const std::string& plaintext,
                             std::vector<uint8_t>& out_blob) {
  if (key32.size() != 32)
    return ESP_ERR_INVALID_ARG;

  uint8_t iv[kIVLen];
  fill_iv(iv);

  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key32.data(), 256);
  if (rc != 0) {
    mbedtls_gcm_free(&ctx);
    return ESP_ERR_INVALID_ARG;
  }

  std::vector<uint8_t> ct(plaintext.size());
  uint8_t tag[kTagLen];

  rc = mbedtls_gcm_crypt_and_tag(
      &ctx, MBEDTLS_GCM_ENCRYPT, plaintext.size(), iv, kIVLen,
      /*aad*/ nullptr, 0, (const uint8_t*)plaintext.data(), ct.data(), kTagLen,
      tag);
  mbedtls_gcm_free(&ctx);
  if (rc != 0)
    return ESP_ERR_INVALID_RESPONSE;

  // Compose blob = magic(4) | ver(1) | iv(12) | tag(16) | ciphertext(...)
  out_blob.clear();
  out_blob.reserve(4 + 1 + kIVLen + kTagLen + ct.size());
  out_blob.insert(out_blob.end(), kMagic, kMagic + 4);
  out_blob.push_back(kVer);
  out_blob.insert(out_blob.end(), iv, iv + kIVLen);
  out_blob.insert(out_blob.end(), tag, tag + kTagLen);
  out_blob.insert(out_blob.end(), ct.begin(), ct.end());
  return ESP_OK;
}

inline esp_err_t gcm_decrypt(const std::vector<uint8_t>& key32,
                             const uint8_t* blob, size_t len,
                             std::string& out_plain) {
  if (key32.size() != 32)
    return ESP_ERR_INVALID_ARG;
  if (len < 4 + 1 + kIVLen + kTagLen)
    return ESP_ERR_INVALID_SIZE;
  if (memcmp(blob, kMagic, 4) != 0)
    return ESP_ERR_INVALID_STATE;

  const uint8_t ver = blob[4];
  (void)ver;  // currently unused
  const uint8_t* iv = blob + 5;
  const uint8_t* tag = iv + kIVLen;
  const uint8_t* ct = tag + kTagLen;
  const size_t ct_len = len - (4 + 1 + kIVLen + kTagLen);

  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key32.data(), 256);
  if (rc != 0) {
    mbedtls_gcm_free(&ctx);
    return ESP_ERR_INVALID_ARG;
  }

  std::vector<uint8_t> pt(ct_len);
  rc =
      mbedtls_gcm_auth_decrypt(&ctx, ct_len, iv, kIVLen,
                               /*aad*/ nullptr, 0, tag, kTagLen, ct, pt.data());
  mbedtls_gcm_free(&ctx);
  if (rc != 0)
    return ESP_ERR_INVALID_RESPONSE;

  out_plain.assign((const char*)pt.data(), pt.size());
  return ESP_OK;
}
// Turn fields <-> cJSON object (values are base64 strings)
inline cJSON* fields_to_json(const std::vector<Field>& fields) {
  cJSON* o = cJSON_CreateObject();
  for (const auto& f : fields) {
    std::string b64 = b64encode(f.value);
    cJSON_AddStringToObject(o, f.name.c_str(), b64.c_str());
  }
  return o;
}
inline std::vector<Field> json_to_fields(cJSON* o) {
  std::vector<Field> out;
  if (!cJSON_IsObject(o))
    return out;
  cJSON* it = nullptr;
  cJSON_ArrayForEach(it, o) {
    if (cJSON_IsString(it) && it->string) {
      Field f;
      f.name = it->string;
      f.value = b64decode(it->valuestring ? it->valuestring : "");
      out.push_back(std::move(f));
    }
  }
  return out;
}

// RAII for NVS handle
class NvsHandle {
 public:
  nvs_handle_t h = 0;
  esp_err_t err = ESP_FAIL;
  explicit NvsHandle(const char* ns, nvs_open_mode_t mode = NVS_READWRITE) {
    err = nvs_open(ns, mode, &h);
    if (err != ESP_OK)
      ESP_LOGE(TAG, "nvs_open %s failed: %s", ns, esp_err_to_name(err));
  }
  ~NvsHandle() {
    if (h)
      nvs_close(h);
  }
  bool ok() const { return err == ESP_OK; }
  operator nvs_handle_t() const { return h; }
};

// Forward decl
inline esp_err_t load_by_suffix(nvs_handle_t handle, const std::string& suf,
                                Record* out);

// ----------------- public API ---------------------------------------------
inline esp_err_t init_nvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  return err;
}

inline esp_err_t save(const Record& r, bool overwrite = true) {
  const std::string suf = key_suffix_for(r.userkey);
  const std::string kname = key_user(suf);
  const std::string kblob = key_blob(suf);

  NvsHandle h(DEFAULT_NS, NVS_READWRITE);
  if (!h.ok())
    return h.err;

  if (!overwrite) {
    size_t len = 0;
    esp_err_t e = nvs_get_str(h, kname.c_str(), nullptr, &len);
    if (e == ESP_OK)
      return ESP_ERR_INVALID_STATE;
  }

  // serialize fields → JSON string
  cJSON* root = fields_to_json(r.fields);
  char* js = cJSON_PrintUnformatted(root);
  if (!js) {
    cJSON_Delete(root);
    return ESP_ERR_NO_MEM;
  }

  esp_err_t err = nvs_set_str(h, kname.c_str(), r.userkey.c_str());
  if (err != ESP_OK) {
    cJSON_free(js);
    cJSON_Delete(root);
    return err;
  }

  err = nvs_set_blob(h, kblob.c_str(), js, strlen(js));
  cJSON_free(js);
  cJSON_Delete(root);
  if (err != ESP_OK)
    return err;
  return nvs_commit(h);
}

inline esp_err_t load(const std::string& userkey, Record* out) {
  if (!out)
    return ESP_ERR_INVALID_ARG;
  const std::string suf = key_suffix_for(userkey);
  const std::string kname = key_user(suf);

  NvsHandle h(DEFAULT_NS, NVS_READONLY);
  if (!h.ok())
    return h.err;

  size_t l = 0;
  esp_err_t err = nvs_get_str(h, kname.c_str(), nullptr, &l);
  if (err != ESP_OK) {
    // Fallback: scan for exact userkey (hash collision/renamed namespace)
    nvs_iterator_t it = nvs_entry_find("nvs", DEFAULT_NS, NVS_TYPE_STR);
    while (it) {
      nvs_entry_info_t info;
      nvs_entry_info(it, &info);
      it = nvs_entry_next(it);
      if (strncmp(info.key, "n_", 2) == 0) {
        size_t len = 0;
        if (nvs_get_str(h, info.key, nullptr, &len) == ESP_OK) {
          std::string u(len, '\0');
          if (nvs_get_str(h, info.key, &u[0], &len) == ESP_OK) {
            strip_nvs_cstr(u, len);
            if (u == userkey) {
              return load_by_suffix(h, std::string(info.key + 2), out);
            }
          }
        }
      }
    }
    return ESP_ERR_NOT_FOUND;
  }
  return load_by_suffix(h, suf, out);
}

inline esp_err_t load_by_suffix(nvs_handle_t handle, const std::string& suf,
                                Record* out) {
  if (!out)
    return ESP_ERR_INVALID_ARG;
  const std::string kname = key_user(suf);
  const std::string kblob = key_blob(suf);

  size_t len = 0;
  esp_err_t err = nvs_get_str(handle, kname.c_str(), nullptr, &len);
  if (err != ESP_OK)
    return err;
  std::string u(len, '\0');
  if ((err = nvs_get_str(handle, kname.c_str(), &u[0], &len)) != ESP_OK)
    return err;
  strip_nvs_cstr(u, len);

  size_t bl = 0;
  if ((err = nvs_get_blob(handle, kblob.c_str(), nullptr, &bl)) != ESP_OK)
    return err;
  std::string js(bl, '\0');
  if ((err = nvs_get_blob(handle, kblob.c_str(), &js[0], &bl)) != ESP_OK)
    return err;

  cJSON* root = cJSON_ParseWithLength(js.data(), js.size());
  if (!root)
    return ESP_FAIL;
  out->userkey = std::move(u);
  out->fields = json_to_fields(root);
  cJSON_Delete(root);
  return ESP_OK;
}

inline bool find_suffix_for_userkey(nvs_handle_t handle,
                                    const std::string& userkey,
                                    std::string& out_suf) {
  nvs_iterator_t it = nvs_entry_find("nvs", DEFAULT_NS, NVS_TYPE_STR);
  while (it) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    it = nvs_entry_next(it);
    if (strncmp(info.key, "n_", 2) == 0) {
      size_t len = 0;
      if (nvs_get_str(handle, info.key, nullptr, &len) == ESP_OK) {
        std::string u(len, '\0');
        if (nvs_get_str(handle, info.key, &u[0], &len) == ESP_OK) {
          strip_nvs_cstr(u, len);
          if (u == userkey) {
            out_suf = std::string(info.key + 2);
            return true;
          }
        }
      }
    }
  }
  return false;
}

inline esp_err_t list(std::vector<std::string>& out_userkeys) {
  out_userkeys.clear();
  NvsHandle h(DEFAULT_NS, NVS_READONLY);
  if (!h.ok())
    return h.err;
  nvs_iterator_t it = nvs_entry_find("nvs", DEFAULT_NS, NVS_TYPE_STR);
  while (it) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    it = nvs_entry_next(it);
    if (strncmp(info.key, "n_", 2) == 0) {
      size_t len = 0;
      if (nvs_get_str(h, info.key, nullptr, &len) == ESP_OK) {
        std::string u(len, '\0');
        if (nvs_get_str(h, info.key, &u[0], &len) == ESP_OK) {
          strip_nvs_cstr(u, len);
          out_userkeys.push_back(std::move(u));
        }
      }
    }
  }
  return ESP_OK;
}

inline esp_err_t search_userkeys(const std::string& query,
                                 std::vector<std::string>& out_userkeys,
                                 bool case_insensitive = true,
                                 bool prefix_only = false) {
  out_userkeys.clear();
  NvsHandle h(DEFAULT_NS, NVS_READONLY);
  if (!h.ok())
    return h.err;
  std::string q = case_insensitive ? to_lower(query) : query;
  nvs_iterator_t it = nvs_entry_find("nvs", DEFAULT_NS, NVS_TYPE_STR);
  while (it) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    it = nvs_entry_next(it);
    if (strncmp(info.key, "n_", 2) == 0) {
      size_t len = 0;
      if (nvs_get_str(h, info.key, nullptr, &len) == ESP_OK) {
        std::string u(len, '\0');
        if (nvs_get_str(h, info.key, &u[0], &len) == ESP_OK) {
          strip_nvs_cstr(u, len);
          std::string cmp = case_insensitive ? to_lower(u) : u;
          bool match = prefix_only
                           ? (cmp.rfind(q, 0) == 0)
                           : (q.empty() || cmp.find(q) != std::string::npos);
          if (match)
            out_userkeys.push_back(std::move(u));
        }
      }
    }
  }
  return ESP_OK;
}

inline bool exists(const std::string& userkey) {
  NvsHandle h(DEFAULT_NS, NVS_READONLY);
  if (!h.ok())
    return false;
  size_t l = 0;
  if (nvs_get_str(h, key_user(key_suffix_for(userkey)).c_str(), nullptr, &l) ==
      ESP_OK) {
    std::string u(l, '\0');
    if (nvs_get_str(h, key_user(key_suffix_for(userkey)).c_str(), &u[0], &l) ==
        ESP_OK) {
      strip_nvs_cstr(u, l);
      if (u == userkey)
        return true;
    }
  }
  std::string suf;
  return find_suffix_for_userkey(h, userkey, suf);
}

inline esp_err_t erase(const std::string& userkey) {
  NvsHandle h(DEFAULT_NS, NVS_READWRITE);
  if (!h.ok())
    return h.err;
  std::string suf;
  if (!find_suffix_for_userkey(h, userkey, suf))
    return ESP_ERR_NOT_FOUND;
  esp_err_t e1 = nvs_erase_key(h, key_user(suf).c_str());
  esp_err_t e2 = nvs_erase_key(h, key_blob(suf).c_str());
  // clear current if pointing here
  char curr[96];
  size_t l = sizeof(curr);
  if (nvs_get_str(h, CURR_KEY, curr, &l) == ESP_OK) {
    std::string cu(curr, curr + (l ? l - 1 : 0));
    if (cu == userkey)
      nvs_erase_key(h, CURR_KEY);
  }
  if (e1 == ESP_OK || e2 == ESP_OK)
    return nvs_commit(h);
  return (e1 != ESP_OK) ? e1 : e2;
}

inline esp_err_t set_current(const std::string& userkey) {
  NvsHandle h(DEFAULT_NS, NVS_READWRITE);
  if (!h.ok())
    return h.err;
  esp_err_t e = nvs_set_str(h, CURR_KEY, userkey.c_str());
  if (e == ESP_OK)
    e = nvs_commit(h);
  return e;
}

inline esp_err_t get_current(Record* out) {
  if (!out)
    return ESP_ERR_INVALID_ARG;
  NvsHandle h(DEFAULT_NS, NVS_READONLY);
  if (!h.ok())
    return h.err;
  size_t l = 0;
  esp_err_t e = nvs_get_str(h, CURR_KEY, nullptr, &l);
  if (e != ESP_OK)
    return e;
  std::string u(l, '\0');
  if ((e = nvs_get_str(h, CURR_KEY, &u[0], &l)) != ESP_OK)
    return e;
  strip_nvs_cstr(u, l);
  return load(u, out);
}

// ------------ field-oriented helpers (modify single field efficiently) -----
inline esp_err_t update_field(const std::string& userkey,
                              const std::string& field_name,
                              const std::vector<uint8_t>& value,
                              bool create_record_if_missing = false) {
  BELL_LOG(info, "Update field '%s' for userkey '%s' in namespace '%s'",
           field_name.c_str(), userkey.c_str(), DEFAULT_NS);
  if (field_name.empty())
    return ESP_ERR_INVALID_ARG;
  NvsHandle h(DEFAULT_NS, NVS_READWRITE);
  if (!h.ok())
    return h.err;

  std::string suf = key_suffix_for(userkey);
  std::string kname = key_user(suf);
  std::string kblob = key_blob(suf);

  // Ensure record exists (or locate by scan)
  size_t l = 0;
  esp_err_t e = nvs_get_str(h, kname.c_str(), nullptr, &l);
  bool found = (e == ESP_OK);
  if (!found) {
    std::string suf2;
    if (find_suffix_for_userkey(h, userkey, suf2)) {
      suf = suf2;
      kname = key_user(suf);
      kblob = key_blob(suf);
      found = true;
    }
  }
  if (!found) {
    if (!create_record_if_missing)
      return ESP_ERR_NOT_FOUND;
    Record r{userkey, {}};
    // create empty then fall through to update
    ESP_RETURN_ON_ERROR(save(r, /*overwrite=*/true), TAG, "save(new)");
  }

  // Load JSON blob (or create empty)
  size_t bl = 0;
  std::string js;
  if (nvs_get_blob(h, kblob.c_str(), nullptr, &bl) == ESP_OK) {
    js.resize(bl);
    ESP_RETURN_ON_ERROR(nvs_get_blob(h, kblob.c_str(), &js[0], &bl), TAG,
                        "get_blob");
  }
  cJSON* root = js.empty() ? cJSON_CreateObject()
                           : cJSON_ParseWithLength(js.data(), js.size());
  if (!root)
    root = cJSON_CreateObject();

  // set/replace field
  std::string b64 = b64encode(value);
  cJSON_ReplaceItemInObject(root, field_name.c_str(),
                            cJSON_CreateString(b64.c_str()));

  // write back
  char* jsw = cJSON_PrintUnformatted(root);
  if (!jsw) {
    cJSON_Delete(root);
    return ESP_ERR_NO_MEM;
  }
  esp_err_t err = nvs_set_str(h, kname.c_str(), userkey.c_str());
  // ensure name
  if (err == ESP_OK)
    err = nvs_set_blob(h, kblob.c_str(), jsw, strlen(jsw));
  cJSON_free(jsw);
  cJSON_Delete(root);
  if (err != ESP_OK)
    return err;
  return nvs_commit(h);
}

inline esp_err_t get_field(const std::string& userkey,
                           const std::string& field_name,
                           std::vector<uint8_t>* out) {
  if (!out || field_name.empty())
    return ESP_ERR_INVALID_ARG;
  Record r;
  esp_err_t e = load(userkey, &r);
  if (e != ESP_OK)
    return e;
  for (auto& f : r.fields) {
    if (f.name == field_name) {
      *out = f.value;
      return ESP_OK;
    }
  }
  return ESP_ERR_NOT_FOUND;
}

// ------------ convenience: strings ----------------------------------------
inline std::vector<uint8_t> as_bytes(const std::string& s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}
inline std::string as_string(const std::vector<uint8_t>& v) {
  return std::string(v.begin(), v.end());
}

inline esp_err_t set_field_str(const std::string& userkey,
                               const std::string& field_name,
                               const std::string& value,
                               bool create_record_if_missing = false) {
  return update_field(userkey, field_name, as_bytes(value),
                      create_record_if_missing);
}
inline esp_err_t get_field_str(const std::string& userkey,
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

// ------------ export / import to SPIFFS -----------------------------------
// Export format: Array of { "userkey": string, "fields": { name: base64 } }
inline esp_err_t export_to_spiffs(const char* path) {
  std::vector<std::string> users;
  ESP_ERROR_CHECK(list(users));
  cJSON* root = cJSON_CreateArray();
  for (auto& u : users) {
    Record r;
    if (load(u, &r) != ESP_OK)
      continue;
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "userkey", r.userkey.c_str());
    cJSON_AddItemToObject(o, "fields", fields_to_json(r.fields));
    cJSON_AddItemToArray(root, o);
  }
  char* js = cJSON_Print(root);
  FILE* f = fopen(path, "w");
  if (!f) {
    cJSON_free(js);
    cJSON_Delete(root);
    return ESP_FAIL;
  }
  fwrite(js, 1, strlen(js), f);
  fclose(f);
  cJSON_free(js);
  cJSON_Delete(root);
  return ESP_OK;
}

inline esp_err_t import_from_spiffs(const char* path, bool overwrite = true) {
  FILE* f = fopen(path, "r");
  if (!f)
    return ESP_FAIL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::string buf;
  buf.resize(n);
  fread(&buf[0], 1, n, f);
  fclose(f);

  cJSON* root = cJSON_ParseWithLength(buf.data(), buf.size());
  if (!root)
    return ESP_FAIL;
  auto handle_obj = [&](cJSON* o) {
    cJSON* uk = cJSON_GetObjectItem(o, "userkey");
    cJSON* fs = cJSON_GetObjectItem(o, "fields");
    if (cJSON_IsString(uk) && (cJSON_IsObject(fs) || fs == nullptr)) {
      Record r;
      r.userkey = uk->valuestring ? uk->valuestring : "";
      r.fields = json_to_fields(fs ? fs : cJSON_CreateObject());
      if (!r.userkey.empty())
        save(r, overwrite);
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

// ------------ startup selection -------------------------------------------
inline esp_err_t get_first(Record* out) {
  if (!out)
    return ESP_ERR_INVALID_ARG;
  NvsHandle h(DEFAULT_NS, NVS_READONLY);
  if (!h.ok())
    return h.err;
  nvs_iterator_t it = nvs_entry_find("nvs", DEFAULT_NS, NVS_TYPE_STR);
  std::string best;
  while (it) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    it = nvs_entry_next(it);
    if (strncmp(info.key, "n_", 2) == 0) {
      std::string suf(info.key + 2);
      if (best.empty() ||
          (std::string("n_") + suf) < (std::string("n_") + best))
        best = std::move(suf);
    }
  }
  if (best.empty())
    return ESP_ERR_NOT_FOUND;
  return load_by_suffix(h, best, out);
}

inline esp_err_t get_startup_record(Record* out, bool set_as_current = true) {
  if (!out)
    return ESP_ERR_INVALID_ARG;
  esp_err_t e = get_current(out);
  if (e == ESP_OK)
    return ESP_OK;
  e = get_first(out);
  if (e == ESP_OK && set_as_current)
    set_current(out->userkey);
  return e;
}

// =========================
// Per-provider NVS namespaces
// =========================
// Use Store to select a custom NVS namespace (e.g., for spotify, soundcloud, etc.)
// Default behavior of the free functions above keeps using DEFAULT_NS ("cred").

class Store {
 public:
  explicit Store(const char* ns = DEFAULT_NS) : ns_(ns) {}

  // -- data ops --
  esp_err_t save(const Record& r, bool overwrite = true) const {
    const std::string suf = key_suffix_for(r.userkey);
    const std::string kname = key_user(suf);
    const std::string kblob = key_blob(suf);
    NvsHandle h(ns_, NVS_READWRITE);
    if (!h.ok())
      return h.err;
    if (!overwrite) {
      size_t len = 0;
      if (nvs_get_str(h, kname.c_str(), nullptr, &len) == ESP_OK)
        return ESP_ERR_INVALID_STATE;
    }
    cJSON* root = fields_to_json(r.fields);
    char* js = cJSON_PrintUnformatted(root);
    if (!js) {
      cJSON_Delete(root);
      return ESP_ERR_NO_MEM;
    }
    esp_err_t err = nvs_set_str(h, kname.c_str(), r.userkey.c_str());
    if (err == ESP_OK)
      err = nvs_set_blob(h, kblob.c_str(), js, strlen(js));
    cJSON_free(js);
    cJSON_Delete(root);
    if (err != ESP_OK)
      return err;
    return nvs_commit(h);
  }

  esp_err_t load(const std::string& userkey, Record* out) const {
    if (!out)
      return ESP_ERR_INVALID_ARG;
    const std::string suf = key_suffix_for(userkey);
    const std::string kname = key_user(suf);
    NvsHandle h(ns_, NVS_READONLY);
    if (!h.ok())
      return h.err;
    size_t l = 0;
    esp_err_t err = nvs_get_str(h, kname.c_str(), nullptr, &l);
    if (err != ESP_OK) {
      nvs_iterator_t it = nvs_entry_find("nvs", ns_, NVS_TYPE_STR);
      while (it) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        it = nvs_entry_next(it);
        if (strncmp(info.key, "n_", 2) == 0) {
          size_t len = 0;
          if (nvs_get_str(h, info.key, nullptr, &len) == ESP_OK) {
            std::string u(len, '\0');
            if (nvs_get_str(h, info.key, &u[0], &len) == ESP_OK) {
              strip_nvs_cstr(u, len);
              if (u == userkey) {
                return load_by_suffix(h, std::string(info.key + 2), out);
              }
            }
          }
        }
      }
      return ESP_ERR_NOT_FOUND;
    }
    return load_by_suffix(h, suf, out);
  }

  esp_err_t list(std::vector<std::string>& out_userkeys) const {
    out_userkeys.clear();
    NvsHandle h(ns_, NVS_READONLY);
    if (!h.ok())
      return h.err;
    nvs_iterator_t it = nvs_entry_find("nvs", ns_, NVS_TYPE_STR);
    while (it) {
      nvs_entry_info_t info;
      nvs_entry_info(it, &info);
      it = nvs_entry_next(it);
      if (strncmp(info.key, "n_", 2) == 0) {
        size_t len = 0;
        if (nvs_get_str(h, info.key, nullptr, &len) == ESP_OK) {
          std::string u(len, '\0');
          if (nvs_get_str(h, info.key, &u[0], &len) == ESP_OK) {
            strip_nvs_cstr(u, len);
            out_userkeys.push_back(std::move(u));
          }
        }
      }
    }
    return ESP_OK;
  }

  esp_err_t search_userkeys(const std::string& query,
                            std::vector<std::string>& out_userkeys,
                            bool case_insensitive = true,
                            bool prefix_only = false) const {
    out_userkeys.clear();
    NvsHandle h(ns_, NVS_READONLY);
    if (!h.ok())
      return h.err;
    std::string q = case_insensitive ? to_lower(query) : query;
    nvs_iterator_t it = nvs_entry_find("nvs", ns_, NVS_TYPE_STR);
    while (it) {
      nvs_entry_info_t info;
      nvs_entry_info(it, &info);
      it = nvs_entry_next(it);
      if (strncmp(info.key, "n_", 2) == 0) {
        size_t len = 0;
        if (nvs_get_str(h, info.key, nullptr, &len) == ESP_OK) {
          std::string u(len, '\0');
          if (nvs_get_str(h, info.key, &u[0], &len) == ESP_OK) {
            strip_nvs_cstr(u, len);
            std::string cmp = case_insensitive ? to_lower(u) : u;
            bool match = prefix_only
                             ? (cmp.rfind(q, 0) == 0)
                             : (q.empty() || cmp.find(q) != std::string::npos);
            if (match)
              out_userkeys.push_back(std::move(u));
          }
        }
      }
    }
    return ESP_OK;
  }

  bool exists(const std::string& userkey) const {
    NvsHandle h(ns_, NVS_READONLY);
    if (!h.ok())
      return false;
    size_t l = 0;
    if (nvs_get_str(h, key_user(key_suffix_for(userkey)).c_str(), nullptr,
                    &l) == ESP_OK) {
      std::string u(l, '\0');
      if (nvs_get_str(h, key_user(key_suffix_for(userkey)).c_str(), &u[0],
                      &l) == ESP_OK) {
        strip_nvs_cstr(u, l);
        if (u == userkey)
          return true;
      }
    }
    std::string suf;
    return find_suffix_for_userkey(h, userkey, suf);
  }

  esp_err_t erase(const std::string& userkey) const {
    NvsHandle h(ns_, NVS_READWRITE);
    if (!h.ok())
      return h.err;
    std::string suf;
    if (!find_suffix_for_userkey(h, userkey, suf))
      return ESP_ERR_NOT_FOUND;
    esp_err_t e1 = nvs_erase_key(h, key_user(suf).c_str());
    esp_err_t e2 = nvs_erase_key(h, key_blob(suf).c_str());
    char curr[96];
    size_t l = sizeof(curr);
    if (nvs_get_str(h, CURR_KEY, curr, &l) == ESP_OK) {
      std::string cu(curr, curr + (l ? l - 1 : 0));
      if (cu == userkey)
        nvs_erase_key(h, CURR_KEY);
    }
    if (e1 == ESP_OK || e2 == ESP_OK)
      return nvs_commit(h);
    return (e1 != ESP_OK) ? e1 : e2;
  }

  esp_err_t set_current(const std::string& userkey) const {
    NvsHandle h(ns_, NVS_READWRITE);
    if (!h.ok())
      return h.err;
    esp_err_t e = nvs_set_str(h, CURR_KEY, userkey.c_str());
    if (e == ESP_OK)
      e = nvs_commit(h);
    return e;
  }

  esp_err_t get_current(Record* out) const {
    if (!out)
      return ESP_ERR_INVALID_ARG;
    NvsHandle h(ns_, NVS_READONLY);
    if (!h.ok())
      return h.err;
    size_t l = 0;
    esp_err_t e = nvs_get_str(h, CURR_KEY, nullptr, &l);
    if (e != ESP_OK)
      return e;
    std::string u(l, '\0');
    if ((e = nvs_get_str(h, CURR_KEY, &u[0], &l)) != ESP_OK)
      return e;
    strip_nvs_cstr(u, l);
    return load(u, out);
  }

  // field helpers
  esp_err_t update_field(const std::string& userkey,
                         const std::string& field_name,
                         const std::vector<uint8_t>& value,
                         bool create_record_if_missing = false) const {
    BELL_LOG(info, "Update field '%s' for userkey '%s' in namespace '%s'",
             field_name.c_str(), userkey.c_str(), ns_);
    if (field_name.empty())
      return ESP_ERR_INVALID_ARG;
    NvsHandle h(ns_, NVS_READWRITE);
    if (!h.ok())
      return h.err;
    std::string suf = key_suffix_for(userkey);
    std::string kname = key_user(suf);
    std::string kblob = key_blob(suf);
    size_t l = 0;
    esp_err_t e = nvs_get_str(h, kname.c_str(), nullptr, &l);
    bool found = (e == ESP_OK);
    if (!found) {
      std::string suf2;
      if (find_suffix_for_userkey(h, userkey, suf2)) {
        suf = suf2;
        kname = key_user(suf);
        kblob = key_blob(suf);
        found = true;
      }
    }
    if (!found) {
      if (!create_record_if_missing)
        return ESP_ERR_NOT_FOUND;
      Record r{userkey, {}};
      ESP_ERROR_CHECK_WITHOUT_ABORT(save(r, /*overwrite=*/true));
    }
    size_t bl = 0;
    std::string js;
    if (nvs_get_blob(h, kblob.c_str(), nullptr, &bl) == ESP_OK) {
      js.resize(bl);
      if (nvs_get_blob(h, kblob.c_str(), &js[0], &bl) != ESP_OK)
        return ESP_FAIL;
    }
    cJSON* root = js.empty() ? cJSON_CreateObject()
                             : cJSON_ParseWithLength(js.data(), js.size());
    if (!root)
      root = cJSON_CreateObject();
    std::string b64 = b64encode(value);
    cJSON_ReplaceItemInObject(root, field_name.c_str(),
                              cJSON_CreateString(b64.c_str()));
    char* jsw = cJSON_PrintUnformatted(root);
    if (!jsw) {
      cJSON_Delete(root);
      return ESP_ERR_NO_MEM;
    }
    esp_err_t err = nvs_set_str(h, kname.c_str(), userkey.c_str());
    if (err == ESP_OK)
      err = nvs_set_blob(h, kblob.c_str(), jsw, strlen(jsw));
    cJSON_free(jsw);
    cJSON_Delete(root);
    if (err != ESP_OK)
      return err;
    return nvs_commit(h);
  }

  esp_err_t get_field(const std::string& userkey, const std::string& field_name,
                      std::vector<uint8_t>* out) const {
    if (!out || field_name.empty())
      return ESP_ERR_INVALID_ARG;
    Record r;
    esp_err_t e = load(userkey, &r);
    if (e != ESP_OK)
      return e;
    for (auto& f : r.fields) {
      if (f.name == field_name) {
        *out = f.value;
        return ESP_OK;
      }
    }
    return ESP_ERR_NOT_FOUND;
  }

  esp_err_t set_field_str(const std::string& userkey,
                          const std::string& field_name,
                          const std::string& value,
                          bool create_record_if_missing = false) const {
    return update_field(userkey, field_name, as_bytes(value),
                        create_record_if_missing);
  }
  esp_err_t get_field_str(const std::string& userkey,
                          const std::string& field_name,
                          std::string* out) const {
    if (!out)
      return ESP_ERR_INVALID_ARG;
    std::vector<uint8_t> v;
    esp_err_t e = get_field(userkey, field_name, &v);
    if (e != ESP_OK)
      return e;
    *out = as_string(v);
    return ESP_OK;
  }

  // export/import confined to this namespace
  esp_err_t export_to_spiffs(const char* path) const {
    std::vector<std::string> users;
    ESP_ERROR_CHECK(list(users));
    cJSON* root = cJSON_CreateArray();
    for (auto& u : users) {
      Record r;
      if (load(u, &r) != ESP_OK)
        continue;
      cJSON* o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "userkey", r.userkey.c_str());
      cJSON_AddItemToObject(o, "fields", fields_to_json(r.fields));
      cJSON_AddItemToArray(root, o);
    }
    char* js = cJSON_Print(root);
    FILE* f = fopen(path, "w");
    if (!f) {
      cJSON_free(js);
      cJSON_Delete(root);
      return ESP_FAIL;
    }
    fwrite(js, 1, strlen(js), f);
    fclose(f);
    cJSON_free(js);
    cJSON_Delete(root);
    return ESP_OK;
  }

  esp_err_t import_from_spiffs(const char* path, bool overwrite = true) const {
    FILE* f = fopen(path, "r");
    if (!f)
      return ESP_FAIL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string buf;
    buf.resize(n);
    fread(&buf[0], 1, n, f);
    fclose(f);
    cJSON* root = cJSON_ParseWithLength(buf.data(), buf.size());
    if (!root)
      return ESP_FAIL;
    auto handle_obj = [&](cJSON* o) {
      cJSON* uk = cJSON_GetObjectItem(o, "userkey");
      cJSON* fs = cJSON_GetObjectItem(o, "fields");
      if (cJSON_IsString(uk) && (cJSON_IsObject(fs) || fs == nullptr)) {
        Record r;
        r.userkey = uk->valuestring ? uk->valuestring : "";
        r.fields = json_to_fields(fs ? fs : cJSON_CreateObject());
        if (!r.userkey.empty())
          save(r, overwrite);
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

  // startup helpers confined to this namespace
  esp_err_t get_first(Record* out) const {
    if (!out)
      return ESP_ERR_INVALID_ARG;
    NvsHandle h(ns_, NVS_READONLY);
    if (!h.ok())
      return h.err;
    nvs_iterator_t it = nvs_entry_find("nvs", ns_, NVS_TYPE_STR);
    std::string best;
    while (it) {
      nvs_entry_info_t info;
      nvs_entry_info(it, &info);
      it = nvs_entry_next(it);
      if (strncmp(info.key, "n_", 2) == 0) {
        std::string suf(info.key + 2);
        if (best.empty() ||
            (std::string("n_") + suf) < (std::string("n_") + best))
          best = std::move(suf);
      }
    }
    if (best.empty())
      return ESP_ERR_NOT_FOUND;
    return load_by_suffix(h, best, out);
  }

  esp_err_t get_startup_record(Record* out, bool set_as_current = true) const {
    if (!out)
      return ESP_ERR_INVALID_ARG;
    esp_err_t e = get_current(out);
    if (e == ESP_OK)
      return ESP_OK;
    e = get_first(out);
    if (e == ESP_OK && set_as_current)
      set_current(out->userkey);
    return e;
  }

  const char* ns() const { return ns_; }

 private:
  const char* ns_;
};

// Convenience factories for common providers (short names keep NVS keyspace tidy)
inline Store spotify_store() {
  return Store("cred_spot");
}
inline Store soundcloud_store() {
  return Store("cred_sc");
}
inline Store yt_store() {
  return Store("cred_yt");
}

// =========================
// SecureStore – AES-256-GCM encrypted-at-rest variant
// =========================
class SecureStore {
 public:
  explicit SecureStore(const char* ns = DEFAULT_NS) : ns_(ns) {}
  // Provide a 32-byte master key
  esp_err_t set_master_key(const uint8_t* key, size_t len) {
    if (!key || len != 32)
      return ESP_ERR_INVALID_ARG;
    master_key_.assign(key, key + len);
    has_key_ = true;
    return ESP_OK;
  }

  // API mirrors Store but always encrypts the JSON blob
  esp_err_t save(const Record& r, bool overwrite = true) const {
    if (!has_key_)
      return ESP_ERR_INVALID_STATE;
    const std::string suf = key_suffix_for(r.userkey);
    const std::string kname = key_user(suf);
    const std::string kblob = key_blob(suf);
    NvsHandle h(ns_, NVS_READWRITE);
    if (!h.ok())
      return h.err;
    if (!overwrite) {
      size_t len = 0;
      if (nvs_get_str(h, kname.c_str(), nullptr, &len) == ESP_OK)
        return ESP_ERR_INVALID_STATE;
    }
    cJSON* root = fields_to_json(r.fields);
    char* js = cJSON_PrintUnformatted(root);
    if (!js) {
      cJSON_Delete(root);
      return ESP_ERR_NO_MEM;
    }
    std::vector<uint8_t> payload;
    esp_err_t ce =
        gcm_encrypt(master_key_, std::string(js, js + strlen(js)), payload);
    cJSON_free(js);
    cJSON_Delete(root);
    if (ce != ESP_OK)
      return ce;
    esp_err_t err = nvs_set_str(h, kname.c_str(), r.userkey.c_str());
    if (err == ESP_OK)
      err = nvs_set_blob(h, kblob.c_str(), payload.data(), payload.size());
    if (err != ESP_OK)
      return err;
    return nvs_commit(h);
  }

  esp_err_t load(const std::string& userkey, Record* out) const {
    if (!out)
      return ESP_ERR_INVALID_ARG;
    if (!has_key_)
      return ESP_ERR_INVALID_STATE;
    const std::string suf = key_suffix_for(userkey);
    const std::string kname = key_user(suf);
    const std::string kblob = key_blob(suf);
    NvsHandle h(ns_, NVS_READONLY);
    if (!h.ok())
      return h.err;
    size_t l = 0;
    esp_err_t err = nvs_get_str(h, kname.c_str(), nullptr, &l);
    std::string suffix = suf;
    if (err != ESP_OK) {
      nvs_iterator_t it = nvs_entry_find("nvs", ns_, NVS_TYPE_STR);
      while (it) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        it = nvs_entry_next(it);
        if (strncmp(info.key, "n_", 2) == 0) {
          size_t len = 0;
          if (nvs_get_str(h, info.key, nullptr, &len) == ESP_OK) {
            std::string u(len, '\0');
            if (nvs_get_str(h, info.key, &u[0], &len) == ESP_OK) {
              strip_nvs_cstr(u, len);
              if (u == userkey) {
                suffix = std::string(info.key + 2);
                goto found;
              }
            }
          }
        }
      }
      return ESP_ERR_NOT_FOUND;
    }
  found: {
    size_t bl = 0;
    if (nvs_get_blob(h, key_blob(suffix).c_str(), nullptr, &bl) != ESP_OK)
      return ESP_ERR_NOT_FOUND;
    std::vector<uint8_t> blob(bl);
    if (nvs_get_blob(h, key_blob(suffix).c_str(), blob.data(), &bl) != ESP_OK)
      return ESP_FAIL;
    if (!blob_is_encrypted(blob.data(), blob.size()))
      return ESP_ERR_INVALID_STATE;
    std::string js;
    esp_err_t de = gcm_decrypt(master_key_, blob.data(), blob.size(), js);
    if (de != ESP_OK)
      return de;
    cJSON* root = cJSON_ParseWithLength(js.data(), js.size());
    if (!root)
      return ESP_FAIL;
    out->userkey = userkey;
    out->fields = json_to_fields(root);
    cJSON_Delete(root);
    return ESP_OK;
  }
  }

  esp_err_t list(std::vector<std::string>& out_userkeys) const {
    out_userkeys.clear();
    NvsHandle h(ns_, NVS_READONLY);
    if (!h.ok())
      return h.err;
    nvs_iterator_t it = nvs_entry_find("nvs", ns_, NVS_TYPE_STR);
    while (it) {
      nvs_entry_info_t info;
      nvs_entry_info(it, &info);
      it = nvs_entry_next(it);
      if (strncmp(info.key, "n_", 2) == 0) {
        size_t len = 0;
        if (nvs_get_str(h, info.key, nullptr, &len) == ESP_OK) {
          std::string u(len, '\0');
          if (nvs_get_str(h, info.key, &u[0], &len) == ESP_OK) {
            strip_nvs_cstr(u, len);
            out_userkeys.push_back(std::move(u));
          }
        }
      }
    }
    return ESP_OK;
  }

  esp_err_t search_userkeys(const std::string& query,
                            std::vector<std::string>& out_userkeys,
                            bool ci = true, bool prefix_only = false) const {
    out_userkeys.clear();
    NvsHandle h(ns_, NVS_READONLY);
    if (!h.ok())
      return h.err;
    std::string q = ci ? to_lower(query) : query;
    nvs_iterator_t it = nvs_entry_find("nvs", ns_, NVS_TYPE_STR);
    while (it) {
      nvs_entry_info_t info;
      nvs_entry_info(it, &info);
      it = nvs_entry_next(it);
      if (strncmp(info.key, "n_", 2) == 0) {
        size_t len = 0;
        if (nvs_get_str(h, info.key, nullptr, &len) == ESP_OK) {
          std::string u(len, '\0');
          if (nvs_get_str(h, info.key, &u[0], &len) == ESP_OK) {
            strip_nvs_cstr(u, len);
            std::string cmp = ci ? to_lower(u) : u;
            bool match = prefix_only
                             ? (cmp.rfind(q, 0) == 0)
                             : (q.empty() || cmp.find(q) != std::string::npos);
            if (match)
              out_userkeys.push_back(std::move(u));
          }
        }
      }
    }
    return ESP_OK;
  }

  bool exists(const std::string& userkey) const {
    NvsHandle h(ns_, NVS_READONLY);
    if (!h.ok())
      return false;
    size_t l = 0;
    if (nvs_get_str(h, key_user(key_suffix_for(userkey)).c_str(), nullptr,
                    &l) == ESP_OK) {
      std::string u(l, '\0');
      if (nvs_get_str(h, key_user(key_suffix_for(userkey)).c_str(), &u[0],
                      &l) == ESP_OK) {
        strip_nvs_cstr(u, l);
        if (u == userkey)
          return true;
      }
    }
    std::string suf;
    return find_suffix_for_userkey(h, userkey, suf);
  }

  esp_err_t erase(const std::string& userkey) const {
    NvsHandle h(ns_, NVS_READWRITE);
    if (!h.ok())
      return h.err;
    std::string suf;
    if (!find_suffix_for_userkey(h, userkey, suf))
      return ESP_ERR_NOT_FOUND;
    esp_err_t e1 = nvs_erase_key(h, key_user(suf).c_str());
    esp_err_t e2 = nvs_erase_key(h, key_blob(suf).c_str());
    if (e1 == ESP_OK || e2 == ESP_OK)
      return nvs_commit(h);
    return (e1 != ESP_OK) ? e1 : e2;
  }

  // field helpers
  esp_err_t update_field(const std::string& userkey,
                         const std::string& field_name,
                         const std::vector<uint8_t>& value,
                         bool create_if_missing = false) const {
    BELL_LOG(info,
             "SecureStore::update_field(userkey='%s', field_name='%s', "
             "value_len=%u, create_if_missing=%d)",
             userkey.c_str(), field_name.c_str(), (unsigned)value.size(),
             (int)create_if_missing);
    if (!has_key_)
      return ESP_ERR_INVALID_STATE;
    if (field_name.empty())
      return ESP_ERR_INVALID_ARG;
    NvsHandle h(ns_, NVS_READWRITE);
    if (!h.ok())
      return h.err;
    std::string suf = key_suffix_for(userkey);
    std::string kname = key_user(suf);
    std::string kblob = key_blob(suf);
    size_t l = 0;
    esp_err_t e = nvs_get_str(h, kname.c_str(), nullptr, &l);
    bool found = (e == ESP_OK);
    if (!found) {
      std::string suf2;
      if (find_suffix_for_userkey(h, userkey, suf2)) {
        suf = suf2;
        kname = key_user(suf);
        kblob = key_blob(suf);
        found = true;
      }
    }
    if (!found) {
      if (!create_if_missing)
        return ESP_ERR_NOT_FOUND;
      Record r{userkey, {}};
      ESP_ERROR_CHECK_WITHOUT_ABORT(save(r, /*overwrite=*/true));
    }
    size_t bl = 0;
    std::vector<uint8_t> blob;
    std::string js;
    if (nvs_get_blob(h, kblob.c_str(), nullptr, &bl) == ESP_OK) {
      blob.resize(bl);
      if (nvs_get_blob(h, kblob.c_str(), blob.data(), &bl) != ESP_OK)
        return ESP_FAIL;
      if (!blob_is_encrypted(blob.data(), blob.size()))
        return ESP_ERR_INVALID_STATE;
      if (gcm_decrypt(master_key_, blob.data(), blob.size(), js) != ESP_OK)
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON* root = js.empty() ? cJSON_CreateObject()
                             : cJSON_ParseWithLength(js.data(), js.size());
    if (!root)
      root = cJSON_CreateObject();
    std::string b64 = b64encode(value);
    cJSON_ReplaceItemInObject(root, field_name.c_str(),
                              cJSON_CreateString(b64.c_str()));
    char* jsw = cJSON_PrintUnformatted(root);
    if (!jsw) {
      cJSON_Delete(root);
      return ESP_ERR_NO_MEM;
    }
    std::vector<uint8_t> payload;
    esp_err_t ce =
        gcm_encrypt(master_key_, std::string(jsw, jsw + strlen(jsw)), payload);
    cJSON_free(jsw);
    cJSON_Delete(root);
    if (ce != ESP_OK)
      return ce;
    esp_err_t err = nvs_set_str(h, kname.c_str(), userkey.c_str());
    if (err == ESP_OK)
      err = nvs_set_blob(h, kblob.c_str(), payload.data(), payload.size());
    if (err != ESP_OK)
      return err;
    return nvs_commit(h);
  }

  esp_err_t get_field(const std::string& userkey, const std::string& field_name,
                      std::vector<uint8_t>* out) const {
    if (!out || field_name.empty())
      return ESP_ERR_INVALID_ARG;
    Record r;
    esp_err_t e = load(userkey, &r);
    if (e != ESP_OK)
      return e;
    for (auto& f : r.fields) {
      if (f.name == field_name) {
        *out = f.value;
        return ESP_OK;
      }
    }
    return ESP_ERR_NOT_FOUND;
  }

  esp_err_t set_field_str(const std::string& userkey,
                          const std::string& field_name,
                          const std::string& value,
                          bool create_if_missing = false) const {
    return update_field(userkey, field_name, as_bytes(value),
                        create_if_missing);
  }
  esp_err_t get_field_str(const std::string& userkey,
                          const std::string& field_name,
                          std::string* out) const {
    if (!out)
      return ESP_ERR_INVALID_ARG;
    std::vector<uint8_t> v;
    esp_err_t e = get_field(userkey, field_name, &v);
    if (e != ESP_OK)
      return e;
    *out = as_string(v);
    return ESP_OK;
  }

  // Export (decrypted JSON)
  esp_err_t export_to_spiffs(const char* path) const {
    std::vector<std::string> users;
    ESP_ERROR_CHECK(list(users));
    cJSON* root = cJSON_CreateArray();
    for (auto& u : users) {
      Record r;
      if (load(u, &r) != ESP_OK)
        continue;
      cJSON* o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "userkey", r.userkey.c_str());
      cJSON_AddItemToObject(o, "fields", fields_to_json(r.fields));
      cJSON_AddItemToArray(root, o);
    }
    char* js = cJSON_Print(root);
    FILE* f = fopen(path, "w");
    if (!f) {
      cJSON_free(js);
      cJSON_Delete(root);
      return ESP_FAIL;
    }
    fwrite(js, 1, strlen(js), f);
    fclose(f);
    cJSON_free(js);
    cJSON_Delete(root);
    return ESP_OK;
  }

  // Import (plaintext JSON) → stored encrypted
  esp_err_t import_from_spiffs(const char* path, bool overwrite = true) const {
    FILE* f = fopen(path, "r");
    if (!f)
      return ESP_FAIL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string buf;
    buf.resize(n);
    fread(&buf[0], 1, n, f);
    fclose(f);
    cJSON* root = cJSON_ParseWithLength(buf.data(), buf.size());
    if (!root)
      return ESP_FAIL;
    auto handle_obj = [&](cJSON* o) {
      cJSON* uk = cJSON_GetObjectItem(o, "userkey");
      cJSON* fs = cJSON_GetObjectItem(o, "fields");
      if (cJSON_IsString(uk) && (cJSON_IsObject(fs) || fs == nullptr)) {
        Record r;
        r.userkey = uk->valuestring ? uk->valuestring : "";
        r.fields = json_to_fields(fs ? fs : cJSON_CreateObject());
        if (!r.userkey.empty())
          save(r, overwrite);
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

  const char* ns() const { return ns_; }

 private:
  const char* ns_;
  bool has_key_ = false;
};

// Convenience factories for encrypted stores per provider
inline SecureStore secure_spotify_store() {
  return SecureStore("cred_spot");
}
inline SecureStore secure_soundcloud_store() {
  return SecureStore("cred_sc");
}
inline SecureStore secure_yt_store() {
  return SecureStore("cred_yt");
}

}  // namespace nvsc
