#pragma once
// AppInitSecureStore.cpp
#include "NvsCredStore.h"
#include "SecureKeyHelper.h"  // sc32_get_master_key(out32)
#include "sc_nvs.h"
#include "BellLogger.h"
constexpr const char* SOUNDCLOUD_CRED = "scloud_cred";
namespace sc_nvs_backend {

static nvsc::SecureStore& sc_store() {
  static auto s = nvsc::secure_soundcloud_store();   // NVS namespace "cred_sc"
  static bool inited = false;
  if (!inited) {
    uint8_t mk[32];
    if (!sc32_get_master_key(mk)) memset(mk, 0, sizeof(mk));
    s.set_master_key(mk, sizeof(mk));
    BELL_LOG(info, "sc_nvs", "Derived SoundCloud master key %02x%02x%02x%02x...%02x%02x%02x%02x",
             mk[0], mk[1], mk[2], mk[3], mk[28], mk[29], mk[30], mk[31]);
    inited = true;
  }
  return s;
}
}


