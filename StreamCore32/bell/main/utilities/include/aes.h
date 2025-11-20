#ifndef _AES_H_
#define _AES_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// #define the macros below to 1/0 to enable/disable the mode of operation.
//
// CBC enables AES encryption in CBC-mode of operation.
// CTR enables encryption in counter-mode.
// ECB enables the basic ECB 16-byte block algorithm. All can be enabled simultaneously.

// The #ifndef-guard allows it to be configured before #include'ing or at compile time.
#ifndef CBC
#define CBC 1
#endif

#ifndef ECB
#define ECB 1
#endif

#ifndef CTR
#define CTR 1
#endif

// Choose key size (enable exactly one).
////#define AES128 1
#define AES192 1
////#define AES256 1

#define AES_BLOCKLEN 16  // AES block size is fixed at 16 bytes

#if defined(AES256) && (AES256 == 1)
  #define AES_KEYLEN 32
  #define AES_keyExpSize 240
#elif defined(AES192) && (AES192 == 1)
  #define AES_KEYLEN 24
  #define AES_keyExpSize 208
#else
  #define AES_KEYLEN 16
  #define AES_keyExpSize 176
#endif

struct AES_ctx {
  uint8_t RoundKey[AES_keyExpSize];
#if (defined(CBC) && (CBC == 1)) || (defined(CTR) && (CTR == 1))
  uint8_t Iv[AES_BLOCKLEN];
#endif
};

void AES_init_ctx(struct AES_ctx* ctx, const uint8_t* key);
#if (defined(CBC) && (CBC == 1)) || (defined(CTR) && (CTR == 1))
void AES_init_ctx_iv(struct AES_ctx* ctx, const uint8_t* key, const uint8_t* iv);
void AES_ctx_set_iv(struct AES_ctx* ctx, const uint8_t* iv);
#endif

#if defined(ECB) && (ECB == 1)
// Buffer size is exactly AES_BLOCKLEN bytes.
// NB: ECB is considered insecure for most uses.
void AES_ECB_encrypt(const struct AES_ctx* ctx, uint8_t* buf);
void AES_ECB_decrypt(const struct AES_ctx* ctx, uint8_t* buf);
#endif

#if defined(CBC) && (CBC == 1)
// Buffer size MUST be a multiple of AES_BLOCKLEN.
void AES_CBC_encrypt_buffer(struct AES_ctx* ctx, uint8_t* buf, size_t length);
void AES_CBC_decrypt_buffer(struct AES_ctx* ctx, uint8_t* buf, size_t length);
#endif

#if defined(CTR) && (CTR == 1)
// Same function for encrypting as for decrypting.
// IV/nonce must never be reused with the same key.
void AES_CTR_xcrypt_buffer(struct AES_ctx* ctx, uint8_t* buf, size_t length);
#endif

#ifdef __cplusplus
} // extern "C"
#endif

#endif  // _AES_H_
