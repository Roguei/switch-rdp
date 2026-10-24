#ifndef RDP_CRYPTO_H
#define RDP_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

/* ---- MD5 ---- */
typedef struct
{
    uint32_t state[4];
    uint64_t count;
    uint8_t  buffer[64];
} md5_ctx;

void md5_init(md5_ctx* ctx);
void md5_update(md5_ctx* ctx, const void* data, size_t len);
void md5_final(md5_ctx* ctx, uint8_t out[16]);
void md5_buf(const void* data, size_t len, uint8_t out[16]);

/* ---- SHA1 ---- */
typedef struct
{
    uint32_t state[5];
    uint64_t count;
    uint8_t  buffer[64];
} sha1_ctx;

void sha1_init(sha1_ctx* ctx);
void sha1_update(sha1_ctx* ctx, const void* data, size_t len);
void sha1_final(sha1_ctx* ctx, uint8_t out[20]);

/* ---- MD4 (NTLM) ---- */
void md4_buf(const void* data, size_t len, uint8_t out[16]);

/* ---- HMAC-MD5 ---- */
void hmac_md5(const uint8_t* key, size_t keylen,
              const uint8_t* msg, size_t msglen, uint8_t out[16]);

/* ---- RC4 ---- */
typedef struct
{
    uint8_t s[256];
    uint8_t i, j;
} rc4_ctx;

void rc4_init(rc4_ctx* c, const uint8_t* key, size_t keylen);
void rc4_crypt(rc4_ctx* c, const uint8_t* in, uint8_t* out, size_t len);

/* ---- RSA public key (big-endian byte strings) ---- */
typedef struct
{
    int      bits;
    uint8_t* modulus;    /* little-endian byte string, bits/8 bytes */
    uint8_t* exponent;   /* little-endian, 4 bytes typically */
} rsa_pubkey;

/* Encrypts msg with PKCS#1 v1.5; returns the cipher length or -1. */
int rsa_encrypt_pkcs1(const rsa_pubkey* key, const uint8_t* msg, int msglen,
                      uint8_t* out);

#endif

