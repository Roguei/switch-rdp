/*
 * rdp_proto - minimal RDP client protocol implementation.
 *
 * Targets "Standard RDP Security" (no NLA, no TLS): the Windows host must
 * have "Allow connections only from computers running Remote Desktop with
 * Network Level Authentication" unchecked.
 *
 * Layers implemented here:
 *   TPKT / X.224 connection
 *   MCS (BER encoded) connect, domain, attach, channel join
 *   Security exchange (RSA encrypted client random) + RC4 keys
 *   Client Info PDU (logon)
 *   Capability exchange
 *   FastPath keyboard input
 *   Update PDUs with bitmap data
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "rdp_crypto.h"

/* ------------------------------------------------------------------ */
/* byte buffer helpers                                                */
/* ------------------------------------------------------------------ */
typedef struct
{
    uint8_t* data;
    size_t   len;
    size_t   cap;
} buf_t;

static void buf_init(buf_t* b, size_t cap)
{
    b->data = (uint8_t*)malloc(cap);
    b->len = 0;
    b->cap = b->data ? cap : 0;
}

static void buf_free(buf_t* b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

static void buf_u8(buf_t* b, uint8_t v)  { if (b->len < b->cap) b->data[b->len++] = v; }
static void buf_u16le(buf_t* b, uint16_t v) { buf_u8(b, (uint8_t)v); buf_u8(b, (uint8_t)(v >> 8)); }
static void buf_u32le(buf_t* b, uint32_t v)
{
    buf_u8(b, (uint8_t)v); buf_u8(b, (uint8_t)(v >> 8));
    buf_u8(b, (uint8_t)(v >> 16)); buf_u8(b, (uint8_t)(v >> 24));
}
static void buf_bytes(buf_t* b, const void* p, size_t n)
{
    if (b->len + n > b->cap) n = b->cap - b->len;
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

/* ---- little endian readers ---- */
static uint16_t rd_u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd_u32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- BER (used by MCS) ---- */
static size_t ber_len_size(size_t len)
{
    if (len < 0x80) return 1;
    if (len < 0x100) return 2;
    if (len < 0x10000) return 3;
    return 4;
}

static void ber_len(buf_t* b, size_t len)
{
    if (len < 0x80) { buf_u8(b, (uint8_t)len); return; }
    if (len < 0x100) { buf_u8(b, 0x81); buf_u8(b, (uint8_t)len); return; }
    if (len < 0x10000)
    {
        buf_u8(b, 0x82); buf_u8(b, (uint8_t)(len >> 8)); buf_u8(b, (uint8_t)len);
        return;
    }
    buf_u8(b, 0x83);
    buf_u8(b, (uint8_t)(len >> 16)); buf_u8(b, (uint8_t)(len >> 8)); buf_u8(b, (uint8_t)len);
}

static size_t ber_open(buf_t* b, uint8_t tag)
{
    buf_u8(b, tag);
    size_t pos = b->len;
    buf_u8(b, 0);  /* placeholder, fixed up by ber_close */
    return pos;
}

static void ber_close(buf_t* b, size_t lenpos)
{
    size_t content = b->len - lenpos - 1;
    if (content < 0x80) { b->data[lenpos] = (uint8_t)content; return; }
    /* shift right to insert extended length */
    size_t extra = ber_len_size(content) - 1;
    if (b->len + extra > b->cap) return;
    memmove(b->data + lenpos + 1 + extra, b->data + lenpos + 1, content);
    b->len += extra;
    size_t p = lenpos;
    if (content < 0x100) { b->data[p++] = 0x81; b->data[p++] = (uint8_t)content; }
    else if (content < 0x10000)
    {
        b->data[p++] = 0x82;
        b->data[p++] = (uint8_t)(content >> 8);
        b->data[p++] = (uint8_t)content;
    }
    else
    {
        b->data[p++] = 0x83;
        b->data[p++] = (uint8_t)(content >> 16);
        b->data[p++] = (uint8_t)(content >> 8);
        b->data[p++] = (uint8_t)content;
    }
}

static void ber_int(buf_t* b, uint32_t v)
{
    buf_u8(b, 0x02);
    if (v < 0x80) { buf_u8(b, 1); buf_u8(b, (uint8_t)v); return; }
    if (v < 0x8000) { buf_u8(b, 2); buf_u8(b, (uint8_t)(v >> 8)); buf_u8(b, (uint8_t)v); return; }
    if (v < 0x800000)
    {
        buf_u8(b, 3); buf_u8(b, (uint8_t)(v >> 16));
        buf_u8(b, (uint8_t)(v >> 8)); buf_u8(b, (uint8_t)v);
        return;
    }
    buf_u8(b, 4);
    buf_u8(b, (uint8_t)(v >> 24)); buf_u8(b, (uint8_t)(v >> 16));
    buf_u8(b, (uint8_t)(v >> 8));  buf_u8(b, (uint8_t)v);
}

static void ber_octet(buf_t* b, const void* p, size_t n)
{
    buf_u8(b, 0x04);
    ber_len(b, n);
    buf_bytes(b, p, n);
}

/* ------------------------------------------------------------------ */
/* connection state                                                   */
/* ------------------------------------------------------------------ */
#define RDP_MAX_CHANNELS 16

typedef struct
{
    int      sock;
    uint16_t user_channel;
    uint16_t io_channel;
    uint16_t channel_ids[RDP_MAX_CHANNELS];
    int      channel_count;

    /* security */
    int      encryption_method;   /* 0 = none, 1 = 40bit, 2 = 128bit */
    int      encryption_level;    /* 1 low, 2 client, 3 high */
    rc4_ctx  rc4_decrypt;
    rc4_ctx  rc4_encrypt;
    uint8_t  mac_salt[32];
    int      use_rc4;

    /* server public key (from the Certificate in the connect response) */
    int      rsa_bits;
    uint8_t  rsa_modulus[256];
    uint32_t rsa_exponent;

    /* desktop */
    int      width, height;

    /* receive buffer */
    uint8_t  rx[65536];
    size_t   rx_len;

    int      connected;
    char     last_error[128];
} rdp_t;

/* ------------------------------------------------------------------ */
/* socket helpers (libnx)                                             */
/* ------------------------------------------------------------------ */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

static int tcp_send_all(int s, const uint8_t* p, size_t n)
{
    while (n)
    {
        int w = (int)send(s, p, n, 0);
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int tcp_recv_some(int s, uint8_t* p, size_t n)
{
    int r = (int)recv(s, p, n, 0);
    return r;
}

/* ------------------------------------------------------------------ */
/* TPKT / X.224                                                       */
/* ------------------------------------------------------------------ */
static int send_x224_connection_request(rdp_t* r)
{
    uint8_t pkt[32];
    size_t n = 0;
    pkt[n++] = 0x03; pkt[n++] = 0x00;      /* TPKT version + reserved */
    pkt[n++] = 0x00; pkt[n++] = 0x00;      /* length, fixed below */
    pkt[n++] = 0x13;                        /* LI */
    pkt[n++] = 0xE0;                        /* CR TPDU */
    pkt[n++] = 0x00; pkt[n++] = 0x00;      /* dst-ref */
    pkt[n++] = 0x00; pkt[n++] = 0x00;      /* src-ref */
    pkt[n++] = 0x00;                        /* class */
    /* RDP negotiation request */
    pkt[n++] = 0x01; pkt[n++] = 0x00; pkt[n++] = 0x08; pkt[n++] = 0x00;
    pkt[n++] = 0x00; pkt[n++] = 0x00; pkt[n++] = 0x00; pkt[n++] = 0x00;
    /* requestedProtocols = PROTOCOL_RDP (0) : plain RDP security */
    pkt[n++] = 0x00; pkt[n++] = 0x00; pkt[n++] = 0x00; pkt[n++] = 0x00;
    pkt[2] = (uint8_t)(n >> 8); pkt[3] = (uint8_t)n;
    return tcp_send_all(r->sock, pkt, n);
}

/* Reads one TPKT frame into buf; returns payload length of the X.224 part. */
static int read_tpkt(rdp_t* r, uint8_t* out, size_t outcap)
{
    uint8_t hdr[4];
    int got = 0;
    while (got < 4)
    {
        int n = tcp_recv_some(r->sock, hdr + got, (size_t)(4 - got));
        if (n <= 0) return -1;
        got += n;
    }
    size_t len = ((size_t)hdr[2] << 8) | hdr[3];
    if (len < 4 || len > outcap) return -1;
    size_t body = len - 4;
    got = 0;
    while ((size_t)got < body)
    {
        int n = tcp_recv_some(r->sock, out + got, body - (size_t)got);
        if (n <= 0) return -1;
        got += n;
    }
    return (int)body;
}
