/*
 * rdp_nla - CredSSP / NTLMv2 for RDP with NLA (MS-CSSP, MS-NLMP).
 */

#include <string.h>
#include <stdlib.h>

#include "rdp_crypto.h"
#include "rdp_tls.h"
#include <stdio.h>
#include "rdp_nla.h"

void (*nla_log)(const char* msg) = NULL;
#define NLOG(...) do { if (nla_log) { char _b[160]; snprintf(_b, sizeof(_b), __VA_ARGS__); nla_log(_b); } } while (0)

/* ------------------------------------------------------------------ */
/* small dynamic buffer                                               */
/* ------------------------------------------------------------------ */
typedef struct { uint8_t* p; size_t len, cap; } b_t;

static void b_init(b_t* b, size_t cap)
{
    b->p = (uint8_t*)malloc(cap);
    b->len = 0;
    b->cap = b->p ? cap : 0;
}
static void b_free(b_t* b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }
static void b_u8(b_t* b, uint8_t v) { if (b->len < b->cap) b->p[b->len++] = v; }
static void b_u16(b_t* b, uint16_t v) { b_u8(b, (uint8_t)v); b_u8(b, (uint8_t)(v >> 8)); }
static void b_u32(b_t* b, uint32_t v)
{
    b_u8(b, (uint8_t)v); b_u8(b, (uint8_t)(v >> 8));
    b_u8(b, (uint8_t)(v >> 16)); b_u8(b, (uint8_t)(v >> 24));
}
static void b_raw(b_t* b, const void* p, size_t n)
{
    if (b->len + n > b->cap) n = b->cap - b->len;
    memcpy(b->p + b->len, p, n);
    b->len += n;
}
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ */
/* UTF8 -> UTF16LE                                                    */
/* ------------------------------------------------------------------ */
static int utf8_to_utf16le(const char* s, uint8_t* out, size_t outcap)
{
    size_t n = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; )
    {
        uint32_t cp;
        if (p[0] < 0x80) { cp = p[0]; p += 1; }
        else if ((p[0] & 0xE0) == 0xC0) { cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((p[0] & 0xF0) == 0xE0)
        {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        }
        else
        {
            cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                 ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            p += 4;
        }
        if (cp < 0x10000)
        {
            if (n + 2 > outcap) break;
            out[n++] = (uint8_t)cp;
            out[n++] = (uint8_t)(cp >> 8);
        }
        else
        {
            cp -= 0x10000;
            uint16_t hi = (uint16_t)(0xD800 + (cp >> 10));
            uint16_t lo = (uint16_t)(0xDC00 + (cp & 0x3FF));
            if (n + 4 > outcap) break;
            out[n++] = (uint8_t)hi; out[n++] = (uint8_t)(hi >> 8);
            out[n++] = (uint8_t)lo; out[n++] = (uint8_t)(lo >> 8);
        }
    }
    return (int)n;
}

/* ------------------------------------------------------------------ */
/* NTLM messages                                                      */
/* ------------------------------------------------------------------ */
#define NTLM_NEGOTIATE_UNICODE      0x00000001u
#define NTLM_REQUEST_TARGET         0x00000004u
#define NTLM_NEGOTIATE_NTLM         0x00000200u
#define NTLM_NEGOTIATE_ALWAYS_SIGN  0x00008000u
#define NTLM_NEGOTIATE_EXT_SESS_SEC 0x00080000u
#define NTLM_NEGOTIATE_TARGET_INFO  0x00800000u
#define NTLM_NEGOTIATE_VERSION      0x02000000u
#define NTLM_NEGOTIATE_128          0x20000000u
#define NTLM_NEGOTIATE_KEY_EXCH     0x40000000u

static int ntlm_build_negotiate(uint8_t* out, size_t cap)
{
    b_t b; b_init(&b, 64);
    b_raw(&b, "NTLMSSP\0", 8);
    b_u32(&b, 1);                                   /* type 1 */
    b_u32(&b, NTLM_NEGOTIATE_UNICODE | NTLM_REQUEST_TARGET |
               NTLM_NEGOTIATE_NTLM | NTLM_NEGOTIATE_ALWAYS_SIGN |
               NTLM_NEGOTIATE_EXT_SESS_SEC | NTLM_NEGOTIATE_TARGET_INFO |
               NTLM_NEGOTIATE_VERSION | NTLM_NEGOTIATE_128);
    b_u16(&b, 0); b_u16(&b, 0); b_u32(&b, 32);      /* domain (empty) */
    b_u16(&b, 0); b_u16(&b, 0); b_u32(&b, 32);      /* workstation (empty) */
    /* version 8 bytes */
    b_u8(&b, 6); b_u8(&b, 1); b_u16(&b, 7601);
    b_u8(&b, 0x0F); b_u8(&b, 0); b_u8(&b, 0); b_u8(&b, 0x0F);
    size_t n = b.len < cap ? b.len : cap;
    memcpy(out, b.p, n);
    b_free(&b);
    return (int)n;
}

typedef struct
{
    uint8_t  server_challenge[8];
    uint8_t  target_info[1024];
    int      target_info_len;
    uint32_t flags;
} ntlm_challenge;

static int ntlm_parse_challenge(const uint8_t* msg, size_t len, ntlm_challenge* out)
{
    if (len < 32) return -1;
    if (memcmp(msg, "NTLMSSP\0", 8) != 0) return -1;
    if (le32(msg + 8) != 2) return -1;

    memcpy(out->server_challenge, msg + 24, 8);
    out->flags = le32(msg + 20);

    out->target_info_len = 0;
    if (len >= 48 && (out->flags & NTLM_NEGOTIATE_TARGET_INFO))
    {
        uint16_t til = le16(msg + 40);
        uint32_t tio = le32(msg + 44);
        if (tio + til <= len && til <= sizeof(out->target_info))
        {
            memcpy(out->target_info, msg + tio, til);
            out->target_info_len = til;
        }
    }
    return 0;
}

static void ntlm_v2_hash(const char* user, const char* domain, const char* pass,
                         uint8_t out[16])
{
    uint8_t nt[16];
    uint8_t u16[512];
    int n = utf8_to_utf16le(pass, u16, sizeof(u16));
    md4_buf(u16, (size_t)n, nt);

    /* identity = UPPER(user) + domain, UTF16LE */
    char ident[256];
    size_t ul = strlen(user);
    if (ul > sizeof(ident) - 2) ul = sizeof(ident) - 2;
    for (size_t i = 0; i < ul; i++)
    {
        char c = user[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        ident[i] = c;
    }
    size_t dl = strlen(domain);
    if (ul + dl >= sizeof(ident)) dl = sizeof(ident) - ul - 1;
    memcpy(ident + ul, domain, dl);
    ident[ul + dl] = 0;

    int m = utf8_to_utf16le(ident, u16, sizeof(u16));
    hmac_md5(nt, 16, u16, (size_t)m, out);
}

static int ntlm_build_authenticate(const nla_cred* cred, const ntlm_challenge* ch,
                                   uint8_t* out, size_t cap,
                                   uint8_t session_key_out[16])
{
    uint8_t v2hash[16];
    ntlm_v2_hash(cred->user, cred->domain, cred->pass, v2hash);

    /* blob */
    uint8_t blob[1200];
    size_t bl = 0;
    blob[bl++] = 0x01; blob[bl++] = 0x01; blob[bl++] = 0x00; blob[bl++] = 0x00;
    memset(blob + bl, 0, 4); bl += 4;                  /* reserved */
    memset(blob + bl, 0, 8); bl += 8;                  /* timestamp (0 => server uses its own) */
    for (int i = 0; i < 8; i++) blob[bl++] = (uint8_t)(0x11 * (i + 1));  /* client nonce */
    memset(blob + bl, 0, 4); bl += 4;
    if (ch->target_info_len > 0 && bl + (size_t)ch->target_info_len < sizeof(blob))
    {
        memcpy(blob + bl, ch->target_info, (size_t)ch->target_info_len);
        bl += (size_t)ch->target_info_len;
    }
    memset(blob + bl, 0, 4); bl += 4;

    /* NT proof string */
    uint8_t proof[16];
    {
        size_t total = 8 + bl;
        uint8_t* tmp = (uint8_t*)malloc(total);
        memcpy(tmp, ch->server_challenge, 8);
        memcpy(tmp + 8, blob, bl);
        hmac_md5(v2hash, 16, tmp, total, proof);
        free(tmp);
    }
    hmac_md5(v2hash, 16, proof, 16, session_key_out);
    memcpy(cred->session_key /* not const-safe */, session_key_out, 16);

    /* NT response = proof + blob */
    uint8_t ntresp[1300];
    memcpy(ntresp, proof, 16);
    memcpy(ntresp + 16, blob, bl);
    size_t ntresp_len = 16 + bl;

    /* LM response: 24 zero bytes */
    uint8_t lmresp[24];
    memset(lmresp, 0, sizeof(lmresp));

    uint8_t dom_u[256], usr_u[256], ws_u[256];
    int dom_l = utf8_to_utf16le(cred->domain, dom_u, sizeof(dom_u));
    int usr_l = utf8_to_utf16le(cred->user, usr_u, sizeof(usr_u));
    int ws_l  = utf8_to_utf16le("SWITCH", ws_u, sizeof(ws_u));

    /* layout: header(64) + dom + usr + ws + lm + nt */
    size_t off = 64;
    size_t dom_off = off; off += (size_t)dom_l;
    size_t usr_off = off; off += (size_t)usr_l;
    size_t ws_off  = off; off += (size_t)ws_l;
    size_t lm_off  = off; off += sizeof(lmresp);
    size_t nt_off  = off; off += ntresp_len;
    if (off > cap) return -1;

    memset(out, 0, off);
    memcpy(out, "NTLMSSP\0", 8);
    out[8] = 3; out[9] = 0; out[10] = 0; out[11] = 0;

    /* security buffers */
    #define SET_SECBUF(pos, len, offset) \
        out[pos] = (uint8_t)(len); out[pos+1] = (uint8_t)((len) >> 8); \
        out[pos+2] = (uint8_t)(len); out[pos+3] = (uint8_t)((len) >> 8); \
        out[pos+4] = (uint8_t)(offset); out[pos+5] = (uint8_t)((offset) >> 8); \
        out[pos+6] = (uint8_t)((offset) >> 16); out[pos+7] = (uint8_t)((offset) >> 24);

    SET_SECBUF(12, sizeof(lmresp), lm_off)
    SET_SECBUF(20, ntresp_len, nt_off)
    SET_SECBUF(28, dom_l, dom_off)
    SET_SECBUF(36, usr_l, usr_off)
    SET_SECBUF(44, ws_l, ws_off)
    SET_SECBUF(52, 0, off)                    /* session key (none) */
    out[60] = 0; out[61] = 0; out[62] = 0; out[63] = 0;

    memcpy(out + dom_off, dom_u, (size_t)dom_l);
    memcpy(out + usr_off, usr_u, (size_t)usr_l);
    memcpy(out + ws_off, ws_u, (size_t)ws_l);
    memcpy(out + lm_off, lmresp, sizeof(lmresp));
    memcpy(out + nt_off, ntresp, ntresp_len);
    #undef SET_SECBUF

    return (int)off;
}

/* ------------------------------------------------------------------ */
/* ASN.1 DER helpers                                                  */
/* ------------------------------------------------------------------ */
static size_t der_len_bytes(size_t len)
{
    if (len < 0x80) return 1;
    if (len < 0x100) return 2;
    if (len < 0x10000) return 3;
    return 4;
}

static void der_put_len(b_t* b, size_t len)
{
    if (len < 0x80) { b_u8(b, (uint8_t)len); return; }
    if (len < 0x100) { b_u8(b, 0x81); b_u8(b, (uint8_t)len); return; }
    if (len < 0x10000)
    {
        b_u8(b, 0x82); b_u8(b, (uint8_t)(len >> 8)); b_u8(b, (uint8_t)len);
        return;
    }
    b_u8(b, 0x83);
    b_u8(b, (uint8_t)(len >> 16)); b_u8(b, (uint8_t)(len >> 8)); b_u8(b, (uint8_t)len);
}

static size_t der_open(b_t* b, uint8_t tag)
{
    b_u8(b, tag);
    size_t pos = b->len;
    b_u8(b, 0);
    return pos;
}

static void der_close(b_t* b, size_t lenpos)
{
    size_t content = b->len - lenpos - 1;
    if (content < 0x80) { b->p[lenpos] = (uint8_t)content; return; }
    size_t need = der_len_bytes(content) - 1;
    if (b->len + need > b->cap) return;
    memmove(b->p + lenpos + 1 + need, b->p + lenpos + 1, content);
    b->len += need;
    size_t save = b->len;
    b->len = lenpos;
    der_put_len(b, content);
    b->len = save;
}

/* ------------------------------------------------------------------ */
/* SPNEGO                                                             */
/* ------------------------------------------------------------------ */
static const uint8_t ntlm_oid[] = { 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x02, 0x0a };

static size_t spnego_wrap_negotiate(const uint8_t* ntlm, size_t ntlm_len,
                                    uint8_t* out, size_t cap)
{
    b_t b; b_init(&b, ntlm_len + 128);
    size_t app = der_open(&b, 0x60);                  /* [APPLICATION 0] */
    size_t seq = der_open(&b, 0x30);
    size_t mech = der_open(&b, 0xA0);                 /* [0] mechTypes */
    size_t list = der_open(&b, 0x30);
    b_u8(&b, 0x06); b_u8(&b, sizeof(ntlm_oid));
    b_raw(&b, ntlm_oid, sizeof(ntlm_oid));
    der_close(&b, list); der_close(&b, mech);
    size_t tok = der_open(&b, 0xA2);                  /* [2] mechToken */
    b_u8(&b, 0x04); der_put_len(&b, ntlm_len);
    b_raw(&b, ntlm, ntlm_len);
    der_close(&b, tok);
    der_close(&b, seq);
    der_close(&b, app);

    size_t n = b.len < cap ? b.len : cap;
    memcpy(out, b.p, n);
    b_free(&b);
    return n;
}

static size_t spnego_wrap_response(const uint8_t* ntlm, size_t ntlm_len,
                                   uint8_t* out, size_t cap)
{
    b_t b; b_init(&b, ntlm_len + 128);
    size_t seq = der_open(&b, 0xA1);                  /* [1] NegTokenResp */
    size_t inner = der_open(&b, 0x30);
    b_u8(&b, 0xA0); b_u8(&b, 0x03); b_u8(&b, 0x0A); b_u8(&b, 0x01); b_u8(&b, 0x01);  /* negState accept-completed */
    if (ntlm && ntlm_len)
    {
        size_t tok = der_open(&b, 0xA2);
        b_u8(&b, 0x04); der_put_len(&b, ntlm_len);
        b_raw(&b, ntlm, ntlm_len);
        der_close(&b, tok);
    }
    der_close(&b, inner);
    der_close(&b, seq);

    size_t n = b.len < cap ? b.len : cap;
    memcpy(out, b.p, n);
    b_free(&b);
    return n;
}

/* Extract the first OCTET STRING payload of an SPNEGO message. */
static int spnego_get_token(const uint8_t* msg, size_t len,
                            const uint8_t** tok, size_t* toklen)
{
    for (size_t i = 0; i + 2 < len; i++)
    {
        if (msg[i] == 0x04)
        {
            size_t l = msg[i+1];
            size_t off = i + 2;
            if (l & 0x80)
            {
                size_t nb = l & 0x7F;
                if (nb == 0 || nb > 3 || i + 2 + nb > len) continue;
                l = 0;
                for (size_t k = 0; k < nb; k++) l = (l << 8) | msg[i + 2 + k];
                off = i + 2 + nb;
            }
            if (off + l <= len && l > 8 && memcmp(msg + off, "NTLMSSP\0", 8) == 0)
            {
                *tok = msg + off;
                *toklen = l;
                return 0;
            }
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* TSRequest                                                          */
/* ------------------------------------------------------------------ */
static size_t tsrequest_build(uint8_t** out, int version,
                              const uint8_t* nego, size_t nego_len,
                              const uint8_t* pubkey, size_t pubkey_len,
                              const uint8_t* nonce, size_t nonce_len)
{
    b_t b; b_init(&b, nego_len + pubkey_len + nonce_len + 128);
    size_t seq = der_open(&b, 0x30);

    size_t v = der_open(&b, 0xA0);
    b_u8(&b, 0x02); b_u8(&b, 0x01); b_u8(&b, (uint8_t)version);
    der_close(&b, v);

    if (nego && nego_len)
    {
        /* Wire format used by mstsc (verified against a capture):
           [1] { SEQUENCE { SEQUENCE { [0] OCTET STRING <raw NTLMSSP> } } } */
        size_t n  = der_open(&b, 0xA1);
        size_t s1 = der_open(&b, 0x30);
        size_t s2 = der_open(&b, 0x30);
        size_t it = der_open(&b, 0xA0);
        b_u8(&b, 0x04); der_put_len(&b, nego_len);
        b_raw(&b, nego, nego_len);
        der_close(&b, it);
        der_close(&b, s2);
        der_close(&b, s1);
        der_close(&b, n);
    }
    if (pubkey && pubkey_len)
    {
        size_t p = der_open(&b, 0xA3);
        b_u8(&b, 0x04); der_put_len(&b, pubkey_len);
        b_raw(&b, pubkey, pubkey_len);
        der_close(&b, p);
    }
    if (nonce && nonce_len)
    {
        size_t n = der_open(&b, 0xA5);
        b_u8(&b, 0x04); der_put_len(&b, nonce_len);
        b_raw(&b, nonce, nonce_len);
        der_close(&b, n);
    }
    der_close(&b, seq);

    *out = b.p;
    return b.len;
}

/* Very small DER scanner: find the payload of the first occurrence of a
   context tag [n] (0xA0|n) whose content is an OCTET STRING. */
static int der_find_octet(const uint8_t* msg, size_t len, uint8_t tag,
                          const uint8_t** payload, size_t* payload_len)
{
    for (size_t i = 0; i + 1 < len; i++)
    {
        if (msg[i] == tag)
        {
            size_t off = i + 1;
            size_t l = msg[off];
            size_t nb = 0;
            if (l & 0x80) { nb = l & 0x7F; l = 0; }
            off++;
            for (size_t k = 0; k < nb; k++) l = (l << 8) | msg[off++];
            if (off + l > len) continue;
            /* inside should be an OCTET STRING */
            if (msg[off] == 0x04)
            {
                size_t ol = msg[off + 1];
                size_t ooff = off + 2;
                if (ol & 0x80)
                {
                    size_t onb = ol & 0x7F; ol = 0;
                    for (size_t k = 0; k < onb; k++) ol = (ol << 8) | msg[ooff++];
                }
                if (ooff + ol <= len)
                {
                    *payload = msg + ooff;
                    *payload_len = ol;
                    return 0;
                }
            }
            *payload = msg + off;
            *payload_len = l;
            return 0;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* CredSSP exchange                                                   */
/* ------------------------------------------------------------------ */
static int tls_write_all(void* tls, const void* p, size_t n)
{
    return rdp_tls_write((rdp_tls_t*)tls, p, n) < 0 ? -1 : 0;
}

static int tls_read_some(void* tls, void* p, size_t n)
{
    return rdp_tls_read((rdp_tls_t*)tls, p, n);
}

int nla_authenticate(void* tls, const nla_cred* cred,
                     const uint8_t* server_pubkey_hash)
{
    (void)server_pubkey_hash;
    uint8_t ntlm[2048];
    uint8_t spnego[2300];
    uint8_t rx[8192];
    int n;

    /* --- 1. NEGOTIATE --- */
    n = ntlm_build_negotiate(ntlm, sizeof(ntlm));
    if (n <= 0) return -1;
    /* raw NTLMSSP goes straight into negoTokens (no SPNEGO wrapper) */
    memcpy(spnego, ntlm, (size_t)n);
    size_t sp = (size_t)n;

    uint8_t* req = NULL;
    size_t reqlen = tsrequest_build(&req, 6, spnego, sp, NULL, 0, NULL, 0);
    if (tls_write_all(tls, req, reqlen) != 0) { free(req); return -1; }
    free(req);

    /* --- 2. CHALLENGE --- */
    int got = tls_read_some(tls, rx, sizeof(rx));
    if (got <= 0) return -1;

    const uint8_t* tok = NULL;
    size_t toklen = 0;
    if (spnego_get_token(rx, (size_t)got, &tok, &toklen) != 0) { NLOG("NLA: no NTLM token in challenge"); return -1; }
    NLOG("NLA: challenge token %d bytes", (int)toklen);

    ntlm_challenge ch;
    if (ntlm_parse_challenge(tok, toklen, &ch) != 0) { NLOG("NLA: challenge parse failed"); return -1; }

    /* --- 3. AUTHENTICATE --- */
    uint8_t session_key[16];
    n = ntlm_build_authenticate(cred, &ch, ntlm, sizeof(ntlm), session_key);
    if (n <= 0) return -1;
    memcpy(spnego, ntlm, (size_t)n);
    sp = (size_t)n;

    reqlen = tsrequest_build(&req, 6, spnego, sp, NULL, 0, NULL, 0);
    if (tls_write_all(tls, req, reqlen) != 0) { free(req); return -1; }
    free(req);

    /* --- 4. server result --- */
    got = tls_read_some(tls, rx, sizeof(rx));
    if (got <= 0) return -1;

    /* an errorCode field [4] != 0 means authentication failed */
    /* Parse the server TSRequest structurally: SEQUENCE of [n] fields.
       [4] errorCode is an INTEGER; absent means success. */
    {
        int code = 0;
        int found = 0;
        size_t q = 0;
        if (got > 2 && rx[0] == 0x30)
        {
            q = 1;
            size_t l = rx[q++];
            if (l & 0x80) { size_t nb = l & 0x7F; l = 0; for (size_t k = 0; k < nb; k++) l = (l << 8) | rx[q++]; }
            size_t stop = q + l;
            if (stop > (size_t)got) stop = (size_t)got;
            while (q + 2 <= stop)
            {
                uint8_t tag = rx[q++];
                size_t cl = rx[q++];
                if (cl & 0x80) { size_t nb = cl & 0x7F; cl = 0; for (size_t k = 0; k < nb && q < stop; k++) cl = (cl << 8) | rx[q++]; }
                if (q + cl > stop) break;
                if (tag == 0xA4 && cl >= 3 && rx[q] == 0x02) { code = rx[q + 2]; found = 1; }
                q += cl;
            }
        }
        NLOG("NLA: errorCode %d%s", code, found ? "" : " (absent)");
        if (code != 0) return -1;
    }
    return 0;
}





