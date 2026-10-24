/*
 * rdp_credssp.c - CredSSP (NLA) client authentication, protocol version 6.
 *
 * Straight port of the Python implementation that is verified against the
 * live Windows server (tools/rdp_client.py):
 *
 *   1. TSRequest[NEGOTIATE]  = NTLMSSP type 1 (raw, no SPNEGO wrapper)
 *   2. read TSRequest        -> NTLMSSP type 2 challenge
 *   3. TSRequest[NEGOTIATE]  = NTLMSSP type 3, plus
 *      TSRequest[pubKeyAuth] = seal( SHA256("CredSSP Client-To-Server Binding
 *                                    Hash\0" || nonce || SPKI_PKCS1) )
 *      TSRequest[nonce]      = 32 random bytes
 *   4. read TSRequest        -> errorCode
 *   5. TSRequest[authInfo]   = seal( TSCredentials(domain, user, password) )
 *   6. read TSRequest        -> errorCode
 *
 * The seal/unseal primitives are the exported-session-key RC4 + HMAC-MD5
 * scheme used by NTLM with SIGN/SEAL/KEYEXCH negotiated.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <mbedtls/sha256.h>

#include "rdp_crypto.h"
#include "rdp_tls.h"
#include "rdp_nla.h"

void (*nla_log)(const char* msg) = NULL;
#define NLOG(...) do { if (nla_log) { char _b[200]; snprintf(_b, sizeof(_b), __VA_ARGS__); nla_log(_b); } } while (0)

#define NEG_FLAGS  0xE28882B7u
#define AUTH_FLAGS 0xE2888235u

static const char CS_MAGIC[] = "CredSSP Client-To-Server Binding Hash";
static const char SC_MAGIC[] = "CredSSP Server-To-Client Binding Hash";

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */
static uint64_t g_rng = 0x243F6A8885A308D3ULL;

static void rnd_bytes(uint8_t* out, size_t n)
{
    static int seeded;
    size_t i;
    if (!seeded)
    {
        seeded = 1;
        g_rng ^= (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ULL;
        g_rng ^= (uint64_t)clock() * 0x2545F4914F6CDD1DULL;
    }
    for (i = 0; i < n; i++)
    {
        g_rng ^= g_rng << 13;
        g_rng ^= g_rng >> 7;
        g_rng ^= g_rng << 17;
        out[i] = (uint8_t)(g_rng >> 33);
    }
}

static int utf16le(const char* s, uint8_t* out, size_t cap)
{
    size_t n = 0;
    if (!s) return 0;
    for (; *s; s++)
    {
        if (n + 2 > cap) break;
        out[n++] = (uint8_t)*s;
        out[n++] = 0;
    }
    return (int)n;
}

static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ------------------------------------------------------------------ */
/* DER buffer                                                          */
/* ------------------------------------------------------------------ */
typedef struct { uint8_t* p; size_t len, cap; } dbuf;

static int d_reserve(dbuf* b, size_t n)
{
    if (b->len + n > b->cap) return -1;
    return 0;
}
static void d_u8(dbuf* b, uint8_t v) { if (!d_reserve(b, 1)) b->p[b->len++] = v; }
static void d_raw(dbuf* b, const void* p, size_t n)
{
    if (!d_reserve(b, n)) { memcpy(b->p + b->len, p, n); b->len += n; }
}
static size_t d_lenbytes(size_t len)
{
    if (len < 0x80) return 1;
    if (len <= 0xFF) return 2;
    if (len <= 0xFFFF) return 3;
    return 4;
}
static void d_putlen(dbuf* b, size_t len)
{
    if (len < 0x80) { d_u8(b, (uint8_t)len); return; }
    if (len <= 0xFF) { d_u8(b, 0x81); d_u8(b, (uint8_t)len); return; }
    if (len <= 0xFFFF) { d_u8(b, 0x82); d_u8(b, (uint8_t)(len >> 8)); d_u8(b, (uint8_t)len); return; }
    d_u8(b, 0x83);
    d_u8(b, (uint8_t)(len >> 16)); d_u8(b, (uint8_t)(len >> 8)); d_u8(b, (uint8_t)len);
}
static size_t d_open(dbuf* b, uint8_t tag)
{
    size_t pos = b->len;
    d_u8(b, tag);
    size_t lp = b->len;
    d_u8(b, 0);                                  /* placeholder */
    (void)lp;
    return pos;
}
/* Re-encode the length of the element starting at `pos`: the content starts
 * at pos+2 and the placeholder byte is only valid for lengths < 0x80, so for
 * larger payloads we shift the content by the extra length bytes. */
static void d_close(dbuf* b, size_t pos)
{
    size_t clen = b->len - pos - 2;
    size_t need = d_lenbytes(clen);
    if (need == 1)
    {
        b->p[pos + 1] = (uint8_t)clen;
        return;
    }
    /* make room for (need-1) extra length bytes right after the tag */
    if (b->len + need - 1 > b->cap) return;
    memmove(b->p + pos + 1 + need, b->p + pos + 2, clen);
    b->len += need - 1;
    memset(b->p + pos + 1, 0, need);
    /* write the length bytes big-endian */
    if (need == 2) b->p[pos + 1] = 0x81;
    else if (need == 3) b->p[pos + 1] = 0x82;
    else b->p[pos + 1] = 0x83;
    {
        size_t k;
        for (k = 0; k < need - 1; k++)
            b->p[pos + 2 + k] = (uint8_t)(clen >> (8 * (need - 2 - k)));
    }
}
static void d_octets(dbuf* b, const uint8_t* p, size_t n)
{
    d_u8(b, 0x04);
    d_putlen(b, n);
    d_raw(b, p, n);
}
static void d_int(dbuf* b, int v)
{
    d_u8(b, 0x02);
    d_putlen(b, 1);
    d_u8(b, (uint8_t)v);
}

/* ------------------------------------------------------------------ */
/* NTLM state                                                          */
/* ------------------------------------------------------------------ */
typedef struct
{
    uint8_t negotiate[64];
    size_t  negotiate_len;

    uint8_t exported[16];
    rc4_ctx send_rc4, recv_rc4;
    uint8_t send_signing[16], recv_signing[16];
    uint32_t send_seq, recv_seq;
} ntlm_t;

static int ntlm_negotiate(ntlm_t* n)
{
    uint8_t* m = n->negotiate;
    memcpy(m, "NTLMSSP\0", 8);
    put32(m + 8, 1);
    put32(m + 12, NEG_FLAGS);
    put32(m + 16, 0); put32(m + 20, 0);
    put32(m + 24, 0); put32(m + 28, 0);
    m[32] = 10; m[33] = 0; put16(m + 34, 26100);
    m[36] = 0; m[37] = 0; m[38] = 0; m[39] = 0x0F;
    n->negotiate_len = 40;
    return 40;
}

typedef struct
{
    uint8_t  server_challenge[8];
    const uint8_t* target_info;
    size_t   target_info_len;
    uint8_t  has_timestamp;
    uint8_t  timestamp[8];
} ntlm_ch;

static int ntlm_parse_challenge(const uint8_t* m, size_t len, ntlm_ch* out)
{
    uint32_t flags;
    uint16_t tilen, timax, tioff;
    size_t q;

    if (len < 48) return -1;
    if (memcmp(m, "NTLMSSP\0", 8) != 0) return -1;
    if ((m[8] | (m[9] << 8) | (m[10] << 16) | (m[11] << 24)) != 2) return -1;

    memcpy(out->server_challenge, m + 24, 8);
    memcpy(&flags, m + 20, 4);
    (void)flags;

    tilen = (uint16_t)(m[40] | (m[41] << 8));
    timax = (uint16_t)(m[42] | (m[43] << 8));
    tioff = (uint16_t)(m[44] | (m[45] << 8));
    (void)timax;
    if ((size_t)tioff + tilen > len) return -1;
    out->target_info = m + tioff;
    out->target_info_len = tilen;

    out->has_timestamp = 0;
    for (q = 0; q + 4 <= out->target_info_len;)
    {
        uint16_t id = (uint16_t)(out->target_info[q] | (out->target_info[q + 1] << 8));
        uint16_t l = (uint16_t)(out->target_info[q + 2] | (out->target_info[q + 3] << 8));
        if (id == 7 && l == 8 && q + 12 <= out->target_info_len)
        {
            memcpy(out->timestamp, out->target_info + q + 4, 8);
            out->has_timestamp = 1;
        }
        q += 4 + l;
        if (id == 0) break;
    }
    return 0;
}

static void ntlm_v2hash(const char* user, const char* domain, const char* pass,
                        uint8_t out[16])
{
    uint8_t nt[16];
    uint8_t u16[512];
    char ident[256];
    size_t ul, dl, i;
    int n, m;

    n = utf16le(pass, u16, sizeof(u16));
    md4_buf(u16, (size_t)n, nt);

    ul = strlen(user);
    if (ul > sizeof(ident) - 2) ul = sizeof(ident) - 2;
    for (i = 0; i < ul; i++)
    {
        char c = user[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        ident[i] = c;
    }
    dl = strlen(domain);
    if (ul + dl >= sizeof(ident)) dl = sizeof(ident) - ul - 1;
    memcpy(ident + ul, domain, dl);
    ident[ul + dl] = 0;
    m = utf16le(ident, u16, sizeof(u16));
    hmac_md5(nt, 16, u16, (size_t)m, out);
}

static void make_keys(ntlm_t* n)
{
    static const char* cs_sign = "session key to client-to-server signing key magic constant";
    static const char* cs_seal = "session key to client-to-server sealing key magic constant";
    static const char* sc_sign = "session key to server-to-client signing key magic constant";
    static const char* sc_seal = "session key to server-to-client sealing key magic constant";
    uint8_t buf[160];

    memcpy(buf, n->exported, 16);
    memcpy(buf + 16, cs_sign, strlen(cs_sign) + 1);
    md5_buf(buf, 16 + strlen(cs_sign) + 1, n->send_signing);

    memcpy(buf + 16, cs_seal, strlen(cs_seal) + 1);
    md5_buf(buf, 16 + strlen(cs_seal) + 1, buf + 32);
    rc4_init(&n->send_rc4, buf + 32, 16);

    memcpy(buf + 16, sc_sign, strlen(sc_sign) + 1);
    md5_buf(buf, 16 + strlen(sc_sign) + 1, n->recv_signing);

    memcpy(buf + 16, sc_seal, strlen(sc_seal) + 1);
    md5_buf(buf, 16 + strlen(sc_seal) + 1, buf + 32);
    rc4_init(&n->recv_rc4, buf + 32, 16);
}

/* Builds the NTLMSSP type 3 message.  `mic_out` receives the MIC computed
 * over negotiate || challenge || message. */
static int ntlm_authenticate(ntlm_t* n, const nla_cred* cred, const ntlm_ch* ch,
                             const uint8_t* challenge_raw, size_t challenge_len,
                             uint8_t* out, size_t cap)
{
    uint8_t v2[16], proof[16], session_base[16];
    uint8_t blob[1400], ti[1200], ntresp[1400];
    uint8_t dom[256], usr[256], ws[256];
    uint8_t rawsess[16], encsess[16];
    size_t bl = 0, til = 0, ntlen;
    int dl, ul, wl;
    size_t off, o_dom, o_usr, o_ws, o_lm, o_nt, o_sk;
    size_t q;

    ntlm_v2hash(cred->user, cred->domain, cred->pass, v2);

    /* target info: copy the server's AV pairs except EOL(0) and AV_FLAGS(6),
     * then mark that a MIC is present and terminate. */
    for (q = 0; q + 4 <= ch->target_info_len;)
    {
        uint16_t id = (uint16_t)(ch->target_info[q] | (ch->target_info[q + 1] << 8));
        uint16_t l = (uint16_t)(ch->target_info[q + 2] | (ch->target_info[q + 3] << 8));
        if (id == 0) break;
        if (id != 6 && til + 4 + l <= sizeof(ti))
        {
            memcpy(ti + til, ch->target_info + q, 4 + (size_t)l);
            til += 4 + (size_t)l;
        }
        q += 4 + (size_t)l;
    }
    ti[til++] = 6; ti[til++] = 0; ti[til++] = 4; ti[til++] = 0;
    ti[til++] = 2; ti[til++] = 0; ti[til++] = 0; ti[til++] = 0;   /* AV_FLAGS = MIC present */
    ti[til++] = 0; ti[til++] = 0; ti[til++] = 0; ti[til++] = 0;   /* EOL */

    blob[bl++] = 1; blob[bl++] = 1; blob[bl++] = 0; blob[bl++] = 0;
    memset(blob + bl, 0, 4); bl += 4;
    if (ch->has_timestamp) memcpy(blob + bl, ch->timestamp, 8);
    else
    {
        uint64_t ft = (uint64_t)((time(NULL) + 11644473600LL) * 10000000LL);
        memcpy(blob + bl, &ft, 8);
    }
    bl += 8;
    rnd_bytes(blob + bl, 8); bl += 8;            /* client challenge */
    memset(blob + bl, 0, 4); bl += 4;
    memcpy(blob + bl, ti, til); bl += til;
    memset(blob + bl, 0, 4); bl += 4;

    {
        uint8_t tmp[1400];
        memcpy(tmp, ch->server_challenge, 8);
        memcpy(tmp + 8, blob, bl);
        hmac_md5(v2, 16, tmp, 8 + bl, proof);
    }
    hmac_md5(v2, 16, proof, 16, session_base);

    memcpy(ntresp, proof, 16);
    memcpy(ntresp + 16, blob, bl);
    ntlen = 16 + bl;

    rnd_bytes(rawsess, 16);
    memcpy(n->exported, rawsess, 16);
    {
        rc4_ctx rc;
        rc4_init(&rc, session_base, 16);
        rc4_crypt(&rc, rawsess, encsess, 16);
    }
    make_keys(n);

    dl = utf16le(cred->domain, dom, sizeof(dom));
    ul = utf16le(cred->user, usr, sizeof(usr));
    wl = utf16le("SWITCH", ws, sizeof(ws));

    off = 88;
    o_dom = off; off += (size_t)dl;
    o_usr = off; off += (size_t)ul;
    o_ws  = off; off += (size_t)wl;
    o_lm  = off; off += 24;
    o_nt  = off; off += ntlen;
    o_sk  = off; off += 16;
    if (off > cap) return -1;

    memset(out, 0, off);
    memcpy(out, "NTLMSSP\0", 8);
    put32(out + 8, 3);
    put16(out + 12, 24);    put16(out + 14, 24);    put32(out + 16, (uint32_t)o_lm);
    put16(out + 20, (uint16_t)ntlen); put16(out + 22, (uint16_t)ntlen);
    put32(out + 24, (uint32_t)o_nt);
    put16(out + 28, (uint16_t)dl);  put16(out + 30, (uint16_t)dl);
    put32(out + 32, (uint32_t)o_dom);
    put16(out + 36, (uint16_t)ul);  put16(out + 38, (uint16_t)ul);
    put32(out + 40, (uint32_t)o_usr);
    put16(out + 44, (uint16_t)wl);  put16(out + 46, (uint16_t)wl);
    put32(out + 48, (uint32_t)o_ws);
    put16(out + 52, 16);            put16(out + 54, 16);
    put32(out + 56, (uint32_t)o_sk);
    put32(out + 60, AUTH_FLAGS);
    out[64] = 10; out[65] = 0; put16(out + 66, 26100);
    out[68] = 0; out[69] = 0; out[70] = 0; out[71] = 0x0F;
    /* bytes 72..87 = MIC (zero for now) */

    memcpy(out + o_dom, dom, (size_t)dl);
    memcpy(out + o_usr, usr, (size_t)ul);
    memcpy(out + o_ws, ws, (size_t)wl);
    memset(out + o_lm, 0, 24);
    memcpy(out + o_nt, ntresp, ntlen);
    memcpy(out + o_sk, encsess, 16);

    {
        /* MIC = HMAC_MD5(exported, negotiate || challenge || message) */
        uint8_t* tmp = (uint8_t*)malloc(n->negotiate_len + challenge_len + off);
        size_t p = 0;
        if (!tmp) return -1;
        memcpy(tmp + p, n->negotiate, n->negotiate_len); p += n->negotiate_len;
        memcpy(tmp + p, challenge_raw, challenge_len); p += challenge_len;
        memcpy(tmp + p, out, off); p += off;
        {
            uint8_t mic[16];
            hmac_md5(n->exported, 16, tmp, p, mic);
            memcpy(out + 72, mic, 16);
        }
        free(tmp);
    }
    return (int)off;
}

/* ------------------------------------------------------------------ */
/* NTLM seal / unseal                                                  */
/* ------------------------------------------------------------------ */
static int ntlm_seal(ntlm_t* n, const uint8_t* data, size_t len, uint8_t* out)
{
    uint8_t digest[16];
    uint8_t* tmp = (uint8_t*)malloc(4 + len);
    uint32_t seq = n->send_seq++;
    if (!tmp) return -1;
    put32(tmp, seq);
    memcpy(tmp + 4, data, len);
    hmac_md5(n->send_signing, 16, tmp, 4 + len, digest);
    free(tmp);

    put32(out, 1);                                 /* signature version */
    rc4_crypt(&n->send_rc4, data, out + 16, len);
    rc4_crypt(&n->send_rc4, digest, out + 4, 8);
    put32(out + 12, seq);
    return (int)(16 + len);
}

static int ntlm_unseal(ntlm_t* n, const uint8_t* in, size_t len, uint8_t* out)
{
    uint8_t digest[16];
    uint8_t* tmp;
    uint32_t seq = n->recv_seq++;
    size_t dlen = (len >= 16) ? len - 16 : 0;

    if (len < 16) return -1;
    rc4_crypt(&n->recv_rc4, in + 16, out, dlen);
    tmp = (uint8_t*)malloc(4 + dlen);
    if (!tmp) return -1;
    put32(tmp, seq);
    memcpy(tmp + 4, out, dlen);
    hmac_md5(n->recv_signing, 16, tmp, 4 + dlen, digest);
    free(tmp);
    {
        uint8_t sync[8];
        rc4_crypt(&n->recv_rc4, digest, sync, 8);  /* keep the RC4 stream aligned */
    }
    return (int)dlen;
}

/* ------------------------------------------------------------------ */
/* TSRequest                                                           */
/* ------------------------------------------------------------------ */
static size_t tsreq_build(uint8_t* out, size_t cap, int version,
                          const uint8_t* nego, size_t nlen,
                          const uint8_t* authinfo, size_t alen,
                          const uint8_t* pubkey, size_t plen,
                          const uint8_t* nonce, size_t noncelen)
{
    dbuf b; size_t seq, t;
    b.p = out; b.len = 0; b.cap = cap;
    seq = d_open(&b, 0x30);
    t = d_open(&b, 0xA0); d_int(&b, version); d_close(&b, t);
    if (nego && nlen)
    {
        t = d_open(&b, 0xA1);
        {
            size_t s1 = d_open(&b, 0x30);
            size_t s2 = d_open(&b, 0x30);
            size_t it = d_open(&b, 0xA0);
            d_octets(&b, nego, nlen);
            d_close(&b, it);
            d_close(&b, s2);
            d_close(&b, s1);
        }
        d_close(&b, t);
    }
    if (authinfo && alen) { t = d_open(&b, 0xA2); d_octets(&b, authinfo, alen); d_close(&b, t); }
    if (pubkey && plen)   { t = d_open(&b, 0xA3); d_octets(&b, pubkey, plen);   d_close(&b, t); }
    if (nonce && noncelen){ t = d_open(&b, 0xA5); d_octets(&b, nonce, noncelen); d_close(&b, t); }
    d_close(&b, seq);
    return b.len;
}

/* Finds the NTLMSSP token inside a TSRequest and reads errorCode from [4]. */
static int find_ntlm(const uint8_t* msg, size_t len, const uint8_t** tok, size_t* toklen)
{
    size_t i;
    for (i = 0; i + 8 <= len; i++)
    {
        if (memcmp(msg + i, "NTLMSSP\0", 8) == 0)
        {
            *tok = msg + i;
            *toklen = len - i;
            return 0;
        }
    }
    return -1;
}

static int parse_error(const uint8_t* msg, size_t len)
{
    size_t q = 0;
    if (len < 4 || msg[0] != 0x30) return 0;
    q = 1;
    {
        size_t l = msg[q++];
        if (l & 0x80) { size_t nb = l & 0x7F, k; l = 0; for (k = 0; k < nb; k++) l = (l << 8) | msg[q++]; }
        {
            size_t stop = q + l;
            if (stop > len) stop = len;
            while (q + 2 <= stop)
            {
                uint8_t tag = msg[q++];
                size_t cl = msg[q++];
                if (cl & 0x80) { size_t nb = cl & 0x7F, k; cl = 0; for (k = 0; k < nb && q < stop; k++) cl = (cl << 8) | msg[q++]; }
                if (q + cl > stop) break;
                if (tag == 0xA4 && cl >= 3 && msg[q] == 0x02) return (int)msg[q + 2];
                q += cl;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main entry                                                          */
/* ------------------------------------------------------------------ */
static int tls_write_all(void* tls, const void* p, size_t n)
{
    return rdp_tls_write((rdp_tls_t*)tls, p, n) < 0 ? -1 : 0;
}
static int tls_read_some(void* tls, void* p, size_t n)
{
    return rdp_tls_read((rdp_tls_t*)tls, p, n);
}

int nla_authenticate(void* tls, const nla_cred* cred, const uint8_t* server_pubkey_hash)
{
    ntlm_t n;
    ntlm_ch ch;
    uint8_t rx[16384], buf[8192], sealed[8192];
    uint8_t pubkey[1024], nonce[32], hash[32], tx[5000];
    size_t pubkey_len = 0;
    int got, rc = -1;
    const uint8_t* tok = NULL;
    size_t toklen = 0;

    (void)server_pubkey_hash;
    memset(&n, 0, sizeof(n));
    memset(&ch, 0, sizeof(ch));

    /* --- 1. NEGOTIATE --- */
    ntlm_negotiate(&n);
    {
        size_t len = tsreq_build(tx, sizeof(tx), 6, n.negotiate, n.negotiate_len,
                                 NULL, 0, NULL, 0, NULL, 0);
        if (tls_write_all(tls, tx, len) != 0) { NLOG("credssp: send negotiate failed"); return -1; }
    }
    NLOG("credssp: negotiate sent (%d B)", (int)n.negotiate_len);

    /* --- 2. CHALLENGE --- */
    got = tls_read_some(tls, rx, sizeof(rx));
    if (got <= 0) { NLOG("credssp: no challenge (%d)", got); return -1; }
    if (find_ntlm(rx, (size_t)got, &tok, &toklen) != 0)
    {
        NLOG("credssp: no NTLM token in challenge");
        return -1;
    }
    /* clip the token to the DER OCTET STRING length: the bytes immediately
     * before the "NTLMSSP" signature are 0x04 <length> */
    {
        size_t off = (size_t)(tok - rx);
        if (off >= 2 && tok[-2] == 0x04 && !(tok[-1] & 0x80))
            toklen = tok[-1];
        else if (off >= 3 && tok[-3] == 0x04 && tok[-2] == 0x81)
            toklen = ((size_t)tok[-1]);
    }
    if (getenv("RDP_NLA_DEBUG"))
    {
        char hx[200];
        size_t k, lim = toklen < 40 ? toklen : 40;
        hx[0] = 0;
        for (k = 0; k < lim; k++)
            sprintf(hx + strlen(hx), "%02x", tok[k]);
        NLOG("credssp: challenge %d B: %s", (int)toklen, hx);
    }
    if (ntlm_parse_challenge(tok, toklen, &ch) != 0) { NLOG("credssp: challenge parse failed"); return -1; }
    NLOG("credssp: challenge ok (ti=%d B, ts=%d)", (int)ch.target_info_len, ch.has_timestamp);

    /* --- 3. AUTHENTICATE + pubKeyAuth --- */
    {
        int alen = ntlm_authenticate(&n, cred, &ch, tok, toklen, buf, sizeof(buf));
        size_t plen;
        if (alen <= 0) { NLOG("credssp: authenticate build failed"); return -1; }
        NLOG("credssp: authenticate %d B", alen);

        if (rdp_tls_server_pubkey((rdp_tls_t*)tls, pubkey, sizeof(pubkey), &pubkey_len) != 0)
        {
            NLOG("credssp: no server pubkey");
            return -1;
        }
        rnd_bytes(nonce, sizeof(nonce));
        {
            mbedtls_sha256_context sc;
            mbedtls_sha256_init(&sc);
            mbedtls_sha256_starts_ret(&sc, 0);
            mbedtls_sha256_update_ret(&sc, (const unsigned char*)CS_MAGIC, sizeof(CS_MAGIC));
            mbedtls_sha256_update_ret(&sc, nonce, sizeof(nonce));
            mbedtls_sha256_update_ret(&sc, pubkey, pubkey_len);
            mbedtls_sha256_finish_ret(&sc, hash);
            mbedtls_sha256_free(&sc);
        }
        plen = (size_t)ntlm_seal(&n, hash, sizeof(hash), sealed);
        {
            size_t len = tsreq_build(tx, sizeof(tx), 6, buf, (size_t)alen,
                                     NULL, 0, sealed, plen, nonce, sizeof(nonce));
            if (tls_write_all(tls, tx, len) != 0) { NLOG("credssp: send auth failed"); return -1; }
        }
        NLOG("credssp: pubkeyauth sent (pubkey %d B)", (int)pubkey_len);
    }

    /* --- 4. server result --- */
    got = tls_read_some(tls, rx, sizeof(rx));
    if (got <= 0) { NLOG("credssp: no server result (%d)", got); return -1; }
    {
        int err = parse_error(rx, (size_t)got);
        NLOG("credssp: pubkeyauth result err=%d (%d B)", err, got);
        if (err != 0) return -1;
    }

    /* --- 5. TSCredentials --- */
    {
        static uint8_t der[2048];
        static uint8_t pwdder[1024];
        dbuf b, pb;
        uint8_t dom[512], usr[512], pwd[512];
        int dl = utf16le(cred->domain, dom, sizeof(dom));
        int ul = utf16le(cred->user, usr, sizeof(usr));
        int wl = utf16le(cred->pass, pwd, sizeof(pwd));
        size_t seq, t;
        size_t slen;

        /* TSPasswordCreds { [0] domain, [1] user, [2] password } */
        pb.p = pwdder; pb.len = 0; pb.cap = sizeof(pwdder);
        seq = d_open(&pb, 0x30);
        t = d_open(&pb, 0xA0); d_octets(&pb, dom, (size_t)dl); d_close(&pb, t);
        t = d_open(&pb, 0xA1); d_octets(&pb, usr, (size_t)ul); d_close(&pb, t);
        t = d_open(&pb, 0xA2); d_octets(&pb, pwd, (size_t)wl); d_close(&pb, t);
        d_close(&pb, seq);

        /* TSCredentials { [0] credType = 1, [1] OCTET STRING TSPasswordCreds } */
        b.p = der; b.len = 0; b.cap = sizeof(der);
        seq = d_open(&b, 0x30);
        t = d_open(&b, 0xA0);
        d_int(&b, 1);
        d_close(&b, t);
        t = d_open(&b, 0xA1);
        d_octets(&b, pb.p, pb.len);
        d_close(&b, t);
        d_close(&b, seq);

        slen = (size_t)ntlm_seal(&n, der, b.len, sealed);
        {
            size_t len = tsreq_build(tx, sizeof(tx), 6, NULL, 0,
                                     sealed, slen, NULL, 0, nonce, sizeof(nonce));
            if (tls_write_all(tls, tx, len) != 0) { NLOG("credssp: send tscredentials failed"); return -1; }
        }
        NLOG("credssp: TSCredentials sent (%d B plain)", (int)b.len);
        if (getenv("RDP_NLA_DEBUG"))
        {
            char hx[200];
            size_t k, lim = b.len < 60 ? b.len : 60;
            hx[0] = 0;
            for (k = 0; k < lim; k++)
                sprintf(hx + strlen(hx), "%02x", der[k]);
            NLOG("credssp: tscred: %s", hx);
        }
    }

    /* --- 6. final result --- */
    got = tls_read_some(tls, rx, sizeof(rx));
    if (got <= 0) { NLOG("credssp: no final result (%d)", got); return -1; }
    {
        int err = parse_error(rx, (size_t)got);
        NLOG("credssp: done err=%d (%d B)", err, got);
        rc = (err == 0) ? 0 : -1;
    }
    return rc;
}
