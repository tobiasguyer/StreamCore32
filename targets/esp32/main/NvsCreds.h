#pragma once

#include <string>
#include <vector>
#include <map>
#include"StreamCoreFile.h"
#include <esp_err.h>
#include "esp_idf_version.h"
#include "nvs.h"
/*
#include "esp_system.h"
static inline esp_err_t nvs_iter_find(const char* part, const char* ns,
                                      nvs_type_t type, nvs_iterator_t* it) {
#if IDF_VERSION_MAJOR >= 5
  return nvs_entry_find(part, ns, type, it);
#else
  *it = nvs_entry_find(part, ns, type);
  return (*it) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
#endif
}

static inline esp_err_t nvs_iter_next(nvs_iterator_t* it) {
#if IDF_VERSION_MAJOR >= 5
  return nvs_entry_next(it);
#else
  *it = nvs_entry_next(*it);
  return (*it) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
#endif
}
*/
esp_err_t init_nvs(const uint8_t* key = nullptr, size_t len = 0);

class Store {
public:
  Store(const char* ns = "cred");
  esp_err_t save(const Record& r, bool overwrite = true);
  esp_err_t load(const std::string& userkey, Record* out);
  esp_err_t list(std::vector<std::string>& out_userkeys);
  esp_err_t search_userkeys(const std::string& query, std::vector<std::string>& out_userkeys, bool case_insensitive = true, bool prefix_only = false);
  esp_err_t exists(const std::string& userkey);
  esp_err_t erase(const std::string& userkey);
  esp_err_t set_current(const std::string& userkey);
  esp_err_t get_current(Record* out);
  esp_err_t update_field(const std::string& userkey, const std::string& field_name, const std::vector<uint8_t>& value, bool create_record_if_missing = false);
  esp_err_t get_field(const std::string& userkey, const std::string& field_name, std::vector<uint8_t>* out);
  esp_err_t set_field_str(const std::string& userkey, const std::string& field_name, const std::string& value, bool create_record_if_missing = false);
  esp_err_t get_field_str(const std::string& userkey, const std::string& field_name, std::string* out);
  esp_err_t export_to_spiffs(const char* path);
  esp_err_t import_from_spiffs(const char* path, bool overwrite = true);
  esp_err_t get_first(Record* out);
  esp_err_t get_startup_record(Record* out, bool set_as_current = true);

private:
  const char* ns_;
};

class SecureStore : public StreamCoreFile {
public:
  SecureStore(const char* ns = "cred");

  int save(const Record& r, bool overwrite = true);
  int load(const std::string& userkey, Record* out);
  int list(std::vector<std::string>& out_userkeys);
  esp_err_t search_userkeys(const std::string& query, std::vector<std::string>& out_userkeys, bool case_insensitive = true, bool prefix_only = false);
  int exists(const std::string& userkey);
  int erase(const std::string& userkey);
  int set_current(const std::string& userkey);
  int get_current(Record* out);
  esp_err_t update_field(const std::string& userkey, const std::string& field_name, const std::vector<uint8_t>& value, bool create_record_if_missing = false);
  esp_err_t get_field(const std::string& userkey, const std::string& field_name, std::vector<uint8_t>* out);
  esp_err_t set_field_str(const std::string& userkey, const std::string& field_name, const std::string& value, bool create_record_if_missing = false);
  esp_err_t get_field_str(const std::string& userkey, const std::string& field_name, std::string* out);
  esp_err_t export_to_spiffs(const char* path);
  esp_err_t import_from_spiffs(const char* path, bool overwrite = true);
  esp_err_t get_first(Record* out);
  esp_err_t get_startup_record(Record* out, bool set_as_current = true);

private:
  const char* ns_;
  esp_err_t has_key_ = false;
};