#pragma once
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include "esp_err.h"
#include "esp_idf_version.h"
#include "mbedtls/gcm.h"
#include "mbedtls/platform.h"
#include "mbedtls/sha256.h"

/* ESP-IDF 5.x: MAC + chip info here */
#include "esp_chip_info.h"  // esp_chip_info_t, esp_chip_info
#include "esp_mac.h"  // esp_base_mac_addr_get, esp_read_mac, ESP_MAC_WIFI_STA

#include "esp_efuse.h"  // esp_efuse_mac_get_default
#include "esp_random.h"
#include "esp_system.h"  // esp_chip_info_t, esp_chip_info

#define mbedtls_sha256_starts_ret mbedtls_sha256_starts
#define mbedtls_sha256_update_ret mbedtls_sha256_update
#define mbedtls_sha256_finish_ret mbedtls_sha256_finish

// Derives a 32-byte key from the device's MAC address and a salt.
bool sc32_get_master_key(uint8_t out32[32]);
// Returns true if the 32-byte key was derived successfully.
static constexpr uint8_t kMagic[4] = {'N', 'V', 'S', 'C'};  // magic
static constexpr uint8_t kVer = 1;                          // format version
static constexpr size_t kIVLen = 12;                        // GCM standard
static constexpr size_t kTagLen = 16;

static void fill_iv(uint8_t iv[kIVLen]) {
  // Nonce/IV choice: for simplicity, pull from mbedtls platform random.
  // Replace with a better RNG if you have one (esp_fill_random).
  esp_fill_random(iv, kIVLen);
}
