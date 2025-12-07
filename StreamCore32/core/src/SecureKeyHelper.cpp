#include "SecureKeyHelper.h"
// You can change this salt for your product / environment.
// Using a product-unique salt makes the derived key app-specific.
static constexpr char kAppSalt[] = "SoundCore32:v1:credstore";

bool sc32_get_master_key(uint8_t out32[32]) {
  uint8_t mac[6] = {0};
  if (esp_efuse_mac_get_default(mac) != ESP_OK) {
    return false;
  }

  // Optional: include chip features into the derivation to widen entropy a bit.
  esp_chip_info_t info{};
  esp_chip_info(&info);

  // Material to hash: MAC || chip_model || cores || features || salt
  uint8_t material[6 + 1 + 1 + 4 + 64];
  size_t pos = 0;
  memcpy(material + pos, mac, 6);
  pos += 6;
  material[pos++] = static_cast<uint8_t>(info.model);
  material[pos++] = static_cast<uint8_t>(info.cores);
  // features is a bitfield; pack it as 4 bytes for stability
  uint32_t features = static_cast<uint32_t>(info.features);
  memcpy(material + pos, &features, sizeof(features));
  pos += sizeof(features);

  constexpr size_t salt_len = sizeof(kAppSalt) - 1;  // no NUL
  static_assert(salt_len <= 64, "kAppSalt longer than expected");

  memcpy(material + pos, kAppSalt, salt_len);
  pos += salt_len;

  // SHA-256(material) -> 32 bytes
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  if (mbedtls_sha256_starts_ret(&ctx, 0) != 0) {  // 0 = SHA-256, 1 = SHA-224
    mbedtls_sha256_free(&ctx);
    return false;
  }
  if (mbedtls_sha256_update_ret(&ctx, material, pos) != 0) {
    mbedtls_sha256_free(&ctx);
    return false;
  }
  if (mbedtls_sha256_finish_ret(&ctx, out32) != 0) {
    mbedtls_sha256_free(&ctx);
    return false;
  }
  mbedtls_sha256_free(&ctx);
  return true;
}