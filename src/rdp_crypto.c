/*
 * rdp_crypto - self-contained crypto primitives for the RDP standard
 * security layer.  No external dependencies.
 *
 * Implements MD5, SHA1, RC4 and RSA public-key encryption (PKCS#1 v1.5)
 * which is exactly what RDP "Standard RDP Security" needs.
 */

#include <stdint.h>
#include <string.h>
#include "rdp_crypto.h"

/* ------------------------------------------------------------------ *
 * MD5
 * ------------------------------------------------------------------ */


static const uint32_t md5_k[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,
    0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
    0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,
    0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,
    0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
    0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,
    0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,
    0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
    0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};

static const uint8_t md5_r[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};

static uint32_t rotl32(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

static void md5_transform(md5_ctx* ctx, const uint8_t block[64])
{
    uint32_t a = ctx->state[0], b = ctx->state[1];
    uint32_t c = ctx->state[2], d = ctx->state[3];
    uint32_t m[16];
    for (int i = 0; i < 16; i++)
        m[i] = (uint32_t)block[i*4] | ((uint32_t)block[i*4+1] << 8) |
               ((uint32_t)block[i*4+2] << 16) | ((uint32_t)block[i*4+3] << 24);

    for (int i = 0; i < 64; i++)
    {
        uint32_t f;
        int g;
        if (i < 16)      { f = (b & c) | (~b & d);        g = i; }
        else if (i < 32) { f = (d & b) | (~d & c);        g = (5*i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;                 g = (3*i + 5) & 15; }
        else             { f = c ^ (b | ~d);              g = (7*i) & 15; }
        uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rotl32(a + f + md5_k[i] + m[g], md5_r[i]);
        a = tmp;
    }
    ctx->state[0] += a; ctx->state[1] += b;
    ctx->state[2] += c; ctx->state[3] += d;
}

void md5_init(md5_ctx* ctx)
{
    ctx->state[0] = 0x67452301; ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe; ctx->state[3] = 0x10325476;
    ctx->count = 0;
}

void md5_update(md5_ctx* ctx, const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    size_t idx = (size_t)(ctx->count & 63);
    ctx->count += len;

    if (idx)
    {
        size_t fill = 64 - idx;
        if (len < fill) { memcpy(ctx->buffer + idx, p, len); return; }
        memcpy(ctx->buffer + idx, p, fill);
        md5_transform(ctx, ctx->buffer);
        p += fill; len -= fill;
    }
    while (len >= 64) { md5_transform(ctx, p); p += 64; len -= 64; }
    if (len) memcpy(ctx->buffer, p, len);
}

void md5_final(md5_ctx* ctx, uint8_t out[16])
{
    uint64_t bits = ctx->count * 8;
    uint8_t pad[72];
    size_t idx = (size_t)(ctx->count & 63);
    size_t padlen = (idx < 56) ? (56 - idx) : (120 - idx);
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    md5_update(ctx, pad, padlen);
    for (int i = 0; i < 8; i++) pad[i] = (uint8_t)(bits >> (8 * i));
    md5_update(ctx, pad, 8);
    for (int i = 0; i < 4; i++)
    {
        out[i*4]   = (uint8_t)(ctx->state[i]);
        out[i*4+1] = (uint8_t)(ctx->state[i] >> 8);
        out[i*4+2] = (uint8_t)(ctx->state[i] >> 16);
        out[i*4+3] = (uint8_t)(ctx->state[i] >> 24);
    }
}

void md5_buf(const void* data, size_t len, uint8_t out[16])
{
    md5_ctx c;
    md5_init(&c);
    md5_update(&c, data, len);
    md5_final(&c, out);
}

/* ------------------------------------------------------------------ *
 * SHA1
 * ------------------------------------------------------------------ */


static void sha1_transform(sha1_ctx* ctx, const uint8_t block[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = rotl32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2];
    uint32_t d = ctx->state[3], e = ctx->state[4];

    for (int i = 0; i < 80; i++)
    {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8f1bbcdc; }
        else             { f = b ^ c ^ d;                    k = 0xca62c1d6; }
        uint32_t tmp = rotl32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl32(b, 30); b = a; a = tmp;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
    ctx->state[3] += d; ctx->state[4] += e;
}

void sha1_init(sha1_ctx* ctx)
{
    ctx->state[0] = 0x67452301; ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE; ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count = 0;
}

void sha1_update(sha1_ctx* ctx, const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    size_t idx = (size_t)(ctx->count & 63);
    ctx->count += len;
    if (idx)
    {
        size_t fill = 64 - idx;
        if (len < fill) { memcpy(ctx->buffer + idx, p, len); return; }
        memcpy(ctx->buffer + idx, p, fill);
        sha1_transform(ctx, ctx->buffer);
        p += fill; len -= fill;
    }
    while (len >= 64) { sha1_transform(ctx, p); p += 64; len -= 64; }
    if (len) memcpy(ctx->buffer, p, len);
}

void sha1_final(sha1_ctx* ctx, uint8_t out[20])
{
    uint64_t bits = ctx->count * 8;
    uint8_t pad[72];
    size_t idx = (size_t)(ctx->count & 63);
    size_t padlen = (idx < 56) ? (56 - idx) : (120 - idx);
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    sha1_update(ctx, pad, padlen);
    for (int i = 0; i < 8; i++) pad[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(ctx, pad, 8);
    for (int i = 0; i < 5; i++)
    {
        out[i*4]   = (uint8_t)(ctx->state[i] >> 24);
        out[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
        out[i*4+2] = (uint8_t)(ctx->state[i] >> 8);
        out[i*4+3] = (uint8_t)(ctx->state[i]);
    }
}

/* ------------------------------------------------------------------ *
 * RC4
 * ------------------------------------------------------------------ */
void rc4_init(rc4_ctx* c, const uint8_t* key, size_t keylen)
{
    for (int i = 0; i < 256; i++) c->s[i] = (uint8_t)i;
    int j = 0;
    for (int i = 0; i < 256; i++)
    {
        j = (j + c->s[i] + key[i % keylen]) & 0xFF;
        uint8_t t = c->s[i]; c->s[i] = c->s[j]; c->s[j] = t;
    }
    c->i = 0;
    c->j = 0;
}

void rc4_crypt(rc4_ctx* c, const uint8_t* in, uint8_t* out, size_t len)
{
    for (size_t n = 0; n < len; n++)
    {
        c->i = (c->i + 1) & 0xFF;
        c->j = (c->j + c->s[c->i]) & 0xFF;
        uint8_t t = c->s[c->i]; c->s[c->i] = c->s[c->j]; c->s[c->j] = t;
        uint8_t k = c->s[(c->s[c->i] + c->s[c->j]) & 0xFF];
        out[n] = in[n] ^ k;
    }
}

/* ------------------------------------------------------------------ *
 * RSA public key operation (encrypt only, PKCS#1 v1.5)
 * Uses mbedTLS bignum: a correct modular exponentiation is not worth
 * re-implementing here.
 * ------------------------------------------------------------------ */

#include <mbedtls/bignum.h>

int rsa_encrypt_pkcs1(const rsa_pubkey* key, const uint8_t* msg, int msglen,
                      uint8_t* out)
{
    int klen = (key->bits + 7) / 8;
    if (msglen > klen - 11) return -1;

    uint8_t block[512];
    if (klen > (int)sizeof(block)) return -1;

    block[0] = 0x00;
    block[1] = 0x02;
    int pslen = klen - 3 - msglen;
    for (int i = 0; i < pslen; i++)
        block[2 + i] = (uint8_t)(0xA5);         /* non-zero padding */
    block[2 + pslen] = 0x00;
    memcpy(block + 3 + pslen, msg, (size_t)msglen);

    int rc = -1;
    mbedtls_mpi N, E, M, R;
    mbedtls_mpi_init(&N); mbedtls_mpi_init(&E);
    mbedtls_mpi_init(&M); mbedtls_mpi_init(&R);

    if (mbedtls_mpi_read_binary(&N, key->modulus, (size_t)klen) == 0 &&
        mbedtls_mpi_read_binary(&E, key->exponent, 4) == 0 &&
        mbedtls_mpi_read_binary(&M, block, (size_t)klen) == 0 &&
        mbedtls_mpi_exp_mod(&R, &M, &E, &N, NULL) == 0)
    {
        size_t olen = 0;
        if (mbedtls_mpi_write_binary(&R, out, (size_t)klen) == 0)
            rc = klen;
    }

    mbedtls_mpi_free(&N); mbedtls_mpi_free(&E);
    mbedtls_mpi_free(&M); mbedtls_mpi_free(&R);
    return rc;
}


/* ------------------------------------------------------------------ *
 * MD4 (required by NTLM)
 * ------------------------------------------------------------------ */
static void md4_transform(uint32_t st[4], const uint8_t block[64])
{
    uint32_t x[16];
    for (int i = 0; i < 16; i++)
        x[i] = (uint32_t)block[i*4] | ((uint32_t)block[i*4+1] << 8) |
               ((uint32_t)block[i*4+2] << 16) | ((uint32_t)block[i*4+3] << 24);
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    #define F(x,y,z) (((x) & (y)) | (~(x) & (z)))
    #define G(x,y,z) (((x) & (y)) | ((x) & (z)) | ((y) & (z)))
    #define H(x,y,z) ((x) ^ (y) ^ (z))
    #define R1(a,b,c,d,k,s) a = rotl32(a + F(b,c,d) + x[k], s)
    #define R2(a,b,c,d,k,s) a = rotl32(a + G(b,c,d) + x[k] + 0x5A827999u, s)
    #define R3(a,b,c,d,k,s) a = rotl32(a + H(b,c,d) + x[k] + 0x6ED9EBA1u, s)

    R1(a,b,c,d, 0, 3); R1(d,a,b,c, 1, 7); R1(c,d,a,b, 2,11); R1(b,c,d,a, 3,19);
    R1(a,b,c,d, 4, 3); R1(d,a,b,c, 5, 7); R1(c,d,a,b, 6,11); R1(b,c,d,a, 7,19);
    R1(a,b,c,d, 8, 3); R1(d,a,b,c, 9, 7); R1(c,d,a,b,10,11); R1(b,c,d,a,11,19);
    R1(a,b,c,d,12, 3); R1(d,a,b,c,13, 7); R1(c,d,a,b,14,11); R1(b,c,d,a,15,19);

    R2(a,b,c,d, 0, 3); R2(d,a,b,c, 4, 5); R2(c,d,a,b, 8, 9); R2(b,c,d,a,12,13);
    R2(a,b,c,d, 1, 3); R2(d,a,b,c, 5, 5); R2(c,d,a,b, 9, 9); R2(b,c,d,a,13,13);
    R2(a,b,c,d, 2, 3); R2(d,a,b,c, 6, 5); R2(c,d,a,b,10, 9); R2(b,c,d,a,14,13);
    R2(a,b,c,d, 3, 3); R2(d,a,b,c, 7, 5); R2(c,d,a,b,11, 9); R2(b,c,d,a,15,13);

    R3(a,b,c,d, 0, 3); R3(d,a,b,c, 8, 9); R3(c,d,a,b, 4,11); R3(b,c,d,a,12,15);
    R3(a,b,c,d, 2, 3); R3(d,a,b,c,10, 9); R3(c,d,a,b, 6,11); R3(b,c,d,a,14,15);
    R3(a,b,c,d, 1, 3); R3(d,a,b,c, 9, 9); R3(c,d,a,b, 5,11); R3(b,c,d,a,13,15);
    R3(a,b,c,d, 3, 3); R3(d,a,b,c,11, 9); R3(c,d,a,b, 7,11); R3(b,c,d,a,15,15);

    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    #undef F
    #undef G
    #undef H
    #undef R1
    #undef R2
    #undef R3
}

void md4_buf(const void* data, size_t len, uint8_t out[16])
{
    uint32_t st[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    const uint8_t* p = (const uint8_t*)data;
    size_t full = len / 64;
    for (size_t i = 0; i < full; i++) md4_transform(st, p + i * 64);

    uint8_t tail[128];
    size_t rem = len % 64;
    memset(tail, 0, sizeof(tail));
    memcpy(tail, p + full * 64, rem);
    tail[rem] = 0x80;
    size_t tl = (rem < 56) ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) tail[tl - 8 + i] = (uint8_t)(bits >> (8 * i));
    md4_transform(st, tail);
    if (tl == 128) md4_transform(st, tail + 64);

    for (int i = 0; i < 4; i++)
    {
        out[i*4]   = (uint8_t)(st[i]);
        out[i*4+1] = (uint8_t)(st[i] >> 8);
        out[i*4+2] = (uint8_t)(st[i] >> 16);
        out[i*4+3] = (uint8_t)(st[i] >> 24);
    }
}

/* ------------------------------------------------------------------ *
 * HMAC-MD5
 * ------------------------------------------------------------------ */
void hmac_md5(const uint8_t* key, size_t keylen,
              const uint8_t* msg, size_t msglen, uint8_t out[16])
{
    uint8_t k[16];
    memset(k, 0, sizeof(k));
    if (keylen > 16) md5_buf(key, keylen, k);
    else memcpy(k, key, keylen);

    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 16; i++)
    {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5C;
    }
    memset(ipad + 16, 0x36, 48);
    memset(opad + 16, 0x5C, 48);

    md5_ctx c;
    uint8_t inner[16];
    md5_init(&c);
    md5_update(&c, ipad, 64);
    md5_update(&c, msg, msglen);
    md5_final(&c, inner);

    md5_init(&c);
    md5_update(&c, opad, 64);
    md5_update(&c, inner, 16);
    md5_final(&c, out);
}
