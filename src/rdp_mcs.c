/*
 * rdp_mcs - MCS (T.125) / GCC (T.124) layer for the switch_rdp client.
 *
 * The MCS envelope is BER encoded; the GCC conference payload carried inside
 * the MCS userData OCTET STRING is PER encoded (the "ALIGNED variant of
 * BASIC-PER" used by RDP, which in practice is a small fixed byte layout).
 *
 * See [MS-RDPBCGR] sections 2.2.1.3 .. 2.2.1.11 for the byte layouts.
 */

#include "rdp_mcs.h"

/* Protocol the server selected in X.224; echoed in the Client Core Data. */
static uint32_t g_selected_protocol;

void rdp_mcs_set_selected_protocol(uint32_t p) { g_selected_protocol = p; }

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* constants                                                          */
/* ------------------------------------------------------------------ */

/* MS-RDPBCGR 2.2.1.11: the user channel id handed out by the Attach User
 * Confirm is userId + 1001, but every outgoing MCS PDU carries the *raw*
 * userId as its initiator field.  Keeping the two straight matters: with a
 * wrong initiator the server rejects every channel join and then silently
 * ignores the Client Info PDU. */
#define MCS_BASE_CHANNEL_ID     1001u   /* userId -> user channel id     */
#define MCS_GLOBAL_CHANNEL_ID   1003u   /* I/O channel id                */

#define MCS_PDU_SEND_DATA_REQ   25u
#define MCS_PDU_SEND_DATA_IND   26u

#define CS_CORE                 0xC001
#define CS_SECURITY             0xC002
#define CS_NET                  0xC003
#define CS_CLUSTER              0xC004

#define SC_CORE                 0x0C01
#define SC_SECURITY             0x0C02
#define SC_NET                  0x0C03

#define TPKT_HEADER_LEN         4u
#define X224_DATA_HEADER_LEN    3u
#define MCS_SEND_DATA_HEADER    (TPKT_HEADER_LEN + X224_DATA_HEADER_LEN + 1u + 2u + 2u + 1u + 2u)

/* ------------------------------------------------------------------ */
/* growable, bounds checked output writer                             */
/* ------------------------------------------------------------------ */

typedef struct
{
    uint8_t* buf;
    size_t   cap;
    size_t   len;
    int      fail;
} mcs_wbuf;

static void w_init(mcs_wbuf* w, uint8_t* buf, size_t cap)
{
    w->buf  = buf;
    w->cap  = cap;
    w->len  = 0;
    w->fail = 0;
}

static void w_put(mcs_wbuf* w, const void* src, size_t n)
{
    if (w->fail) return;

    if (n > w->cap - w->len)
    {
        w->fail = 1;
        return;
    }

    if (n > 0)
        memcpy(w->buf + w->len, src, n);

    w->len += n;
}

static void w_u8(mcs_wbuf* w, uint8_t v)
{
    w_put(w, &v, 1);
}

static void w_u16le(mcs_wbuf* w, uint16_t v)
{
    uint8_t b[2];
    b[0] = (uint8_t)(v & 0xFFu);
    b[1] = (uint8_t)(v >> 8);
    w_put(w, b, sizeof(b));
}

static void w_u16be(mcs_wbuf* w, uint16_t v)
{
    uint8_t b[2];
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)(v & 0xFFu);
    w_put(w, b, sizeof(b));
}

static void w_u32le(mcs_wbuf* w, uint32_t v)
{
    uint8_t b[4];
    b[0] = (uint8_t)(v & 0xFFu);
    b[1] = (uint8_t)((v >> 8) & 0xFFu);
    b[2] = (uint8_t)((v >> 16) & 0xFFu);
    b[3] = (uint8_t)((v >> 24) & 0xFFu);
    w_put(w, b, sizeof(b));
}

static void w_zeros(mcs_wbuf* w, size_t n)
{
    if (w->fail) return;

    if (n > w->cap - w->len)
    {
        w->fail = 1;
        return;
    }

    memset(w->buf + w->len, 0, n);
    w->len += n;
}

/* Open a BER TLV: write the tag plus a one byte length placeholder.
   Returns the position of the placeholder. */
static size_t w_ber_open(mcs_wbuf* w, uint8_t tag)
{
    size_t pos;

    w_u8(w, tag);
    pos = w->len;
    w_u8(w, 0);
    return pos;
}

static size_t w_ber_open_app(mcs_wbuf* w, uint8_t tag)
{
    size_t pos;

    w_u8(w, 0x7Fu);
    w_u8(w, tag);
    pos = w->len;
    w_u8(w, 0);
    return pos;
}

/* Fix up the length placeholder written by w_ber_open(), inserting the
   extended form when required. */
static void w_ber_close(mcs_wbuf* w, size_t lenpos)
{
    uint8_t hdr[3];
    size_t  content;
    size_t  hlen;
    size_t  extra;

    if (w->fail) return;
    if (lenpos + 1u > w->len)
    {
        w->fail = 1;
        return;
    }

    content = w->len - lenpos - 1u;

    if (content < 0x80u)
    {
        w->buf[lenpos] = (uint8_t)content;
        return;
    }

    if (content < 0x100u)
    {
        hdr[0] = 0x81u;
        hdr[1] = (uint8_t)content;
        hlen   = 2;
    }
    else if (content < 0x10000u)
    {
        hdr[0] = 0x82u;
        hdr[1] = (uint8_t)(content >> 8);
        hdr[2] = (uint8_t)(content & 0xFFu);
        hlen   = 3;
    }
    else
    {
        w->fail = 1;
        return;
    }

    extra = hlen - 1u;
    if (extra > w->cap - w->len)
    {
        w->fail = 1;
        return;
    }

    memmove(w->buf + lenpos + hlen, w->buf + lenpos + 1u, content);
    memcpy(w->buf + lenpos, hdr, hlen);
    w->len += extra;
}

/* BER INTEGER.  Values are unsigned, but DER requires a leading 0x00 when
   the most significant bit is set (Windows' BER parser rejects the negative
   form). */
static void w_ber_uint(mcs_wbuf* w, uint32_t v)
{
    w_u8(w, 0x02u);

    if (v <= 0xFFu)
    {
        if (v & 0x80u) { w_u8(w, 2); w_u8(w, 0); }
        else           { w_u8(w, 1); }
        w_u8(w, (uint8_t)v);
    }
    else if (v <= 0xFFFFu)
    {
        if (v & 0x8000u) { w_u8(w, 3); w_u8(w, 0); }
        else             { w_u8(w, 2); }
        w_u16be(w, (uint16_t)v);
    }
    else if (v <= 0xFFFFFFu)
    {
        if (v & 0x800000u) { w_u8(w, 4); w_u8(w, 0); }
        else               { w_u8(w, 3); }
        w_u8(w, (uint8_t)(v >> 16));
        w_u16be(w, (uint16_t)(v & 0xFFFFu));
    }
    else
    {
        w_u8(w, 4);
        w_u8(w, (uint8_t)(v >> 24));
        w_u8(w, (uint8_t)((v >> 16) & 0xFFu));
        w_u8(w, (uint8_t)((v >> 8) & 0xFFu));
        w_u8(w, (uint8_t)(v & 0xFFu));
    }
}

/* PER length determinant as used by the GCC/MCS PDUs.  Only the one and
   two byte forms are needed (the two byte form holds a 15 bit length). */
static size_t per_len_bytes(size_t n)
{
    return (n <= 0x7Fu) ? 1u : 2u;
}

static void w_per_len(mcs_wbuf* w, size_t n)
{
    if (n <= 0x7Fu)
    {
        w_u8(w, (uint8_t)n);
    }
    else if (n <= 0x7FFFu)
    {
        w_u16be(w, (uint16_t)(0x8000u | (uint32_t)n));
    }
    else
    {
        w->fail = 1;
    }
}

/* ------------------------------------------------------------------ */
/* little endian readers                                              */
/* ------------------------------------------------------------------ */

static uint16_t get_le16(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_le32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t get_be16(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* ------------------------------------------------------------------ */
/* client data blocks                                                 */
/* ------------------------------------------------------------------ */

static int build_client_data(uint8_t* buf, size_t cap,
                             uint16_t width, uint16_t height,
                             const char* channel_names, int channel_count,
                             const char* client_name)
{
    mcs_wbuf w;
    uint8_t  name16[32];
    size_t   start;
    int      i;

    w_init(&w, buf, cap);

    /* ---- Client Core Data (TS_UD_CS_CORE) ---- */
    start = w.len;
    w_u16le(&w, CS_CORE);
    w_u16le(&w, 234);                 /* 4 byte header + 230 byte payload */
    w_u32le(&w, 0x00080004u);         /* version (RDP 5.0) */
    w_u16le(&w, width);
    w_u16le(&w, height);
    w_u16le(&w, 0xCA01u);             /* colorDepth: RNS_UD_COLOR_8BPP sentinel */
    w_u16le(&w, 0xAA03u);             /* SASSequence */
    w_u32le(&w, 0x00000409u);         /* keyboardLayout: en-US */
    w_u32le(&w, 2600u);               /* clientBuild */

    memset(name16, 0, sizeof(name16));
    if (client_name == NULL || client_name[0] == '\0')
        client_name = "SWITCH";
    for (i = 0; i < 15 && client_name[i] != '\0'; i++)
    {
        name16[i * 2]     = (uint8_t)client_name[i];
        name16[i * 2 + 1] = 0;
    }
    w_put(&w, name16, sizeof(name16)); /* clientName, UTF-16LE, 32 bytes */

    w_u32le(&w, 4);                   /* keyboardType: IBM enhanced */
    w_u32le(&w, 0);                   /* keyboardSubType */
    w_u32le(&w, 12);                  /* keyboardFunctionKey */
    w_zeros(&w, 64);                  /* imeFileName */
    w_u16le(&w, 0xCA03u);             /* postBeta2ColorDepth: 24 bpp */
    w_u16le(&w, 1);                   /* clientProductId */
    w_u32le(&w, 0);                   /* serialNumber */
    w_u16le(&w, 0x0018u);             /* highColorDepth: 24 bpp */
    w_u16le(&w, 0x000Eu);             /* supportedColorDepths: 24/16/15 */
    w_u16le(&w, 0x0001u);             /* earlyCapabilityFlags: ERRINFO PDU */
    w_zeros(&w, 64);                  /* clientDigProductId */
    w_u8(&w, 0);                      /* connectionType */
    w_u8(&w, 0);                      /* pad1octet */
    w_u32le(&w, g_selected_protocol); /* serverSelectedProtocol */
    w_u32le(&w, (uint32_t)width);     /* desktopPhysicalWidth */
    w_u32le(&w, (uint32_t)height);    /* desktopPhysicalHeight */
    w_u16le(&w, 0);                   /* desktopOrientation */
    w_u32le(&w, 100);                 /* desktopScaleFactor */
    w_u32le(&w, 100);                 /* deviceScaleFactor */
    if (w.fail || w.len - start != 234u)
        return -1;

    /* ---- Client Security Data (TS_UD_CS_SEC) ----
       Enhanced RDP Security (NLA/TLS) is used, so no RC4 encryption is
       advertised. */
    start = w.len;
    w_u16le(&w, CS_SECURITY);
    w_u16le(&w, 12);
    w_u32le(&w, 0);                   /* encryptionMethods */
    w_u32le(&w, 0);                   /* extEncryptionMethods */
    if (w.fail || w.len - start != 12u)
        return -1;

    /* ---- Client Network Data (TS_UD_CS_NET) ---- */
    start = w.len;
    w_u16le(&w, CS_NET);
    w_u16le(&w, (uint16_t)(8u + (size_t)channel_count * 12u));
    w_u32le(&w, (uint32_t)channel_count);
    for (i = 0; i < channel_count; i++)
    {
        uint8_t name8[8];
        const char* src = channel_names + (size_t)i * 8u;
        int k;

        memset(name8, 0, sizeof(name8));
        for (k = 0; k < 7 && src[k] != '\0'; k++)
            name8[k] = (uint8_t)src[k];
        w_put(&w, name8, sizeof(name8));
        w_u32le(&w, 0x80800000u);     /* INITIALIZED | ENCRYPT_RDP | ... */
    }
    if (w.fail || w.len - start != 8u + (size_t)channel_count * 12u)
        return -1;

    /* ---- Client Cluster Data (TS_UD_CS_CLUSTER) ----
       mstsc sends this block last; Windows has been observed to drop a
       Connect Initial whose blocks are ordered differently. */
    start = w.len;
    w_u16le(&w, CS_CLUSTER);
    w_u16le(&w, 12);
    w_u32le(&w, 0x00000001u);         /* cluster flags: REDIRECTION_VERSION1 */
    w_u32le(&w, 0);                   /* RedirectSessionId */
    if (w.fail || w.len - start != 12u)
        return -1;

    if (w.len > 0xFFFFu)
        return -1;

    return (int)w.len;
}

static void build_domain_params(mcs_wbuf* w,
                                uint32_t max_channels, uint32_t max_users,
                                uint32_t max_tokens, uint32_t max_pdu)
{
    size_t pos = w_ber_open(w, 0x30u); /* DomainParameters ::= SEQUENCE */

    w_ber_uint(w, max_channels);
    w_ber_uint(w, max_users);
    w_ber_uint(w, max_tokens);
    w_ber_uint(w, 1);                  /* numPriorities */
    w_ber_uint(w, 0);                  /* minThroughput */
    w_ber_uint(w, 1);                  /* maxHeight */
    w_ber_uint(w, max_pdu);            /* maxPDUSize */
    w_ber_uint(w, 2);                  /* protocolVersion */

    w_ber_close(w, pos);
}

/* ------------------------------------------------------------------ */
/* MCS Connect Initial                                                */
/* ------------------------------------------------------------------ */

int rdp_mcs_build_connect_initial(uint8_t* out, size_t outcap,
                                  int width, int height, int color_depth,
                                  const char* channel_names, int channel_count,
                                  const char* client_name)
{
    static const uint8_t gcc_oid[7] =
        { 0x00, 0x05, 0x00, 0x14, 0x7Cu, 0x00, 0x01 };
    static const uint8_t domain_selector[3] = { 0x04, 0x01, 0x01 };
    static const uint8_t upward_true[3]     = { 0x01, 0x01, 0xFF };

    uint8_t  blocks[1024];
    uint8_t  gcc[1024];
    int      block_len;
    size_t   connect_len;
    size_t   tlen_pos;
    size_t   app_pos;
    size_t   ud_pos;
    mcs_wbuf w;
    mcs_wbuf g;

    (void)color_depth; /* the actual depth is advertised through highColorDepth */

    if (out == NULL)
        return -1;
    if (width < 1 || width > 0xFFFF || height < 1 || height > 0xFFFF)
        return -1;
    if (channel_count < 0 || channel_count > RDP_MCS_MAX_CHANNELS)
        return -1;
    if (channel_count > 0 && channel_names == NULL)
        return -1;

    block_len = build_client_data(blocks, sizeof(blocks),
                                  (uint16_t)width, (uint16_t)height,
                                  channel_names, channel_count, client_name);
    if (block_len < 0)
        return -1;

    /* ---- GCC Conference Create Request (PER encoded) ---- */
    w_init(&g, gcc, sizeof(gcc));

    w_put(&g, gcc_oid, sizeof(gcc_oid)); /* T.124 object identifier */

    /* Length of the ConnectGCCPDU content that follows.  It consists of the
       12 fixed bytes below plus the userData length determinant and the
       client data blocks. */
    connect_len = 12u + per_len_bytes((size_t)block_len) + (size_t)block_len;
    w_per_len(&g, connect_len);

    w_u8(&g, 0x00);                   /* conferenceCreateRequest */
    w_u8(&g, 0x08);                   /* optional userData present */
    w_u8(&g, 0x00);                   /* ConferenceName::numeric length = 0 + 1 */
    w_u8(&g, 0x10);                   /* numeric string "1" */
    w_u8(&g, 0x00);                   /* padding */
    w_u8(&g, 0x01);                   /* one UserData set */
    w_u8(&g, 0xC0);                   /* value present, h221NonStandard key */
    w_u8(&g, 0x00);                   /* h221NonStandard length = 0 + 4 */
    w_put(&g, "Duca", 4);             /* client-to-server H.221 key */

    w_per_len(&g, (size_t)block_len);
    w_put(&g, blocks, (size_t)block_len);

    if (g.fail)
        return -1;

    /* ---- TPKT + X.224 Data TPDU + MCS Connect Initial ---- */
    w_init(&w, out, outcap);
    w_u8(&w, 0x03);                   /* TPKT version */
    w_u8(&w, 0x00);                   /* reserved */
    tlen_pos = w.len;
    w_u16be(&w, 0);                   /* total length, fixed below */
    w_u8(&w, 0x02);                   /* X.224 LI */
    w_u8(&w, 0xF0);                   /* Data TPDU */
    w_u8(&w, 0x80);                   /* EOT */

    app_pos = w_ber_open_app(&w, 0x65); /* [APPLICATION 101] ConnectInitial */

    w_put(&w, domain_selector, sizeof(domain_selector)); /* callingDomainSelector */
    w_put(&w, domain_selector, sizeof(domain_selector)); /* calledDomainSelector */
    w_put(&w, upward_true, sizeof(upward_true));         /* upwardFlag = TRUE */

    /* target / minimum / maximum DomainParameters */
    build_domain_params(&w, 34, 2, 0, 0xFFFFu);
    build_domain_params(&w, 1, 1, 1, 0x0420u);
    build_domain_params(&w, 0xFFFFu, 0xFC17u, 0xFFFFu, 0xFFFFu);

    ud_pos = w_ber_open(&w, 0x04);    /* userData OCTET STRING */
    w_put(&w, gcc, g.len);
    w_ber_close(&w, ud_pos);
    w_ber_close(&w, app_pos);

    if (w.fail || w.len > 0xFFFFu)
        return -1;

    w.buf[tlen_pos]     = (uint8_t)(w.len >> 8);
    w.buf[tlen_pos + 1] = (uint8_t)(w.len & 0xFFu);

    return (int)w.len;
}

/* ------------------------------------------------------------------ */
/* bounds checked input reader                                        */
/* ------------------------------------------------------------------ */

typedef struct
{
    const uint8_t* p;
    size_t         len;
    size_t         pos;
} mcs_rdr;

static int rd_u8(mcs_rdr* r, uint8_t* v)
{
    if (r->pos >= r->len)
        return -1;

    *v = r->p[r->pos++];
    return 0;
}

static int rd_skip(mcs_rdr* r, size_t n)
{
    if (n > r->len - r->pos)
        return -1;

    r->pos += n;
    return 0;
}

/* BER definite length (short or 0x81..0x84 long form). */
static int rd_ber_len(mcs_rdr* r, size_t* out)
{
    uint8_t b;
    size_t  n;
    size_t  v;

    if (rd_u8(r, &b) != 0)
        return -1;

    if ((b & 0x80u) == 0)
    {
        *out = b;
        return 0;
    }

    n = (size_t)(b & 0x7Fu);
    if (n == 0 || n > 4)
        return -1;

    v = 0;
    while (n-- > 0)
    {
        uint8_t c;
        if (rd_u8(r, &c) != 0)
            return -1;
        v = (v << 8) | c;
    }

    *out = v;
    return 0;
}

/* PER length determinant (one byte, or two bytes with the high bit set). */
static int rd_per_len(mcs_rdr* r, size_t* out)
{
    uint8_t b;

    if (rd_u8(r, &b) != 0)
        return -1;

    if ((b & 0x80u) == 0)
    {
        *out = b;
        return 0;
    }

    {
        uint8_t c;
        if (rd_u8(r, &c) != 0)
            return -1;
        *out = ((size_t)(b & 0x7Fu) << 8) | c;
    }
    return 0;
}

/* Removes an optional TPKT header and an optional X.224 Data TPDU. */
static int strip_transport_headers(mcs_rdr* r)
{
    if (r->len >= TPKT_HEADER_LEN && r->p[0] == 0x03u && r->p[1] == 0x00u)
    {
        size_t total = ((size_t)r->p[2] << 8) | (size_t)r->p[3];

        if (total < TPKT_HEADER_LEN)
            return -1;

        total -= TPKT_HEADER_LEN;
        if (total > r->len - TPKT_HEADER_LEN)
            total = r->len - TPKT_HEADER_LEN; /* tolerate a short capture */

        r->p += TPKT_HEADER_LEN;
        r->len = total;
        r->pos = 0;
    }

    if (r->len >= 3u && r->p[1] == 0xF0u)
    {
        size_t li = r->p[0];

        if (li < 1u || li + 1u > r->len)
            return -1;

        r->p += li + 1u;
        r->len -= li + 1u;
        r->pos = 0;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* GCC Conference Create Response helpers                             */
/* ------------------------------------------------------------------ */

static int block_type_known(uint16_t type)
{
    switch (type)
    {
        case 0x0C01:
        case 0x0C02:
        case 0x0C03:
        case 0x0C04:
        case 0x0C05:
        case 0x0C06:
        case 0x0C07:
        case 0x0C08:
        case 0x0C09:
        case 0x0C0A:
            return 1;
        default:
            return 0;
    }
}

/* Locate the server data blocks inside the GCC Conference Create Response
   by walking its PER encoding. */
static int gcc_find_blocks(const uint8_t* p, size_t n,
                           const uint8_t** blocks, size_t* blocks_len)
{
    mcs_rdr  r;
    uint8_t  tag;
    size_t   tmp;
    size_t   i;
    uint8_t  set_count;
    const uint8_t* found     = NULL;
    size_t   found_len       = 0;

    if (n < 8u)
        return -1;

    /* t124Identifier: the reference encoding starts with 0x00 (CHOICE
       "object"), followed by the five byte OID body.  Accept a leading
       0x06 as well, since some descriptions show the BER tag here. */
    if (!(p[0] == 0x00u || p[0] == 0x06u))
        return -1;
    if (!(p[1] == 0x05u && p[2] == 0x00u && p[3] == 0x14u &&
          p[4] == 0x7Cu && p[5] == 0x00u && p[6] == 0x01u))
        return -1;

    r.p   = p;
    r.len = n;
    r.pos = 7u;

    if (rd_per_len(&r, &tmp) != 0)    /* ConnectData::connectPDU length */
        return -1;

    if (rd_u8(&r, &tag) != 0)
        return -1;
    if ((tag >> 2) != 5u)             /* conferenceCreateResponse */
        return -1;

    if (rd_skip(&r, 2) != 0)          /* nodeID */
        return -1;

    if (rd_per_len(&r, &tmp) != 0)    /* tag (INTEGER) */
        return -1;
    if (rd_skip(&r, tmp) != 0)
        return -1;

    if (rd_skip(&r, 1) != 0)          /* result (ENUMERATED) */
        return -1;

    if (rd_u8(&r, &set_count) != 0)
        return -1;
    if (set_count == 0 || set_count > 16u)
        return -1;

    for (i = 0; i < (size_t)set_count; i++)
    {
        uint8_t choice;
        size_t  key_len;
        size_t  value_len;

        if (rd_u8(&r, &choice) != 0)
            return -1;

        if ((choice & 0x80u) == 0)
            continue;                 /* UserData::value not present */

        if (rd_per_len(&r, &key_len) != 0) /* h221NonStandard identifier */
            return -1;
        if (rd_skip(&r, key_len) != 0)
            return -1;

        if (rd_per_len(&r, &value_len) != 0)
            return -1;
        if (value_len > r.len - r.pos)
            return -1;

        if (value_len > found_len)
        {
            found     = r.p + r.pos;
            found_len = value_len;
        }

        if (rd_skip(&r, value_len) != 0)
            return -1;
    }

    if (found == NULL || found_len == 0)
        return -1;

    *blocks     = found;
    *blocks_len = found_len;
    return 0;
}

/* Fallback for responses whose GCC wrapper is not exactly the reference
   layout: search for the start of a self-consistent chain of server data
   blocks that ends at the end of the userData. */
static int gcc_scan_blocks(const uint8_t* p, size_t n,
                           const uint8_t** blocks, size_t* blocks_len)
{
    size_t i;

    for (i = 0; i + 4u <= n; i++)
    {
        size_t off;
        int    count = 0;
        int    ok    = 1;

        if (!block_type_known(get_le16(p + i)))
            continue;

        off = i;
        while (off + 4u <= n)
        {
            uint16_t type  = get_le16(p + off);
            uint16_t blen  = get_le16(p + off + 2u);

            if (!block_type_known(type) || blen < 4u ||
                (size_t)blen > n - off)
            {
                ok = 0;
                break;
            }

            off += blen;
            count++;
        }

        if (ok && count > 0 && off == n)
        {
            *blocks     = p + i;
            *blocks_len = n - i;
            return 0;
        }
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/* server data blocks                                                 */
/* ------------------------------------------------------------------ */

static uint32_t normalize_exponent(uint32_t e)
{
    /* 65537 is always stored as 01 00 01 00 (little-endian) by Windows.
       Accept the byte swapped form too, in case a server emits the field in
       network byte order. */
    if (e > 0xFFFFu)
    {
        uint32_t sw = ((e & 0x000000FFu) << 24) |
                      ((e & 0x0000FF00u) << 8)  |
                      ((e & 0x00FF0000u) >> 8)  |
                      ((e & 0xFF000000u) >> 24);
        if (sw == 3u || sw == 17u || sw == 65537u)
            e = sw;
    }
    return e;
}

/* Number of significant bits of a big-endian integer. */
static size_t modulus_bit_length(const uint8_t* p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
    {
        if (p[i] != 0u)
        {
            uint8_t b    = p[i];
            size_t  bits = (n - i - 1u) * 8u;

            while (b != 0u)
            {
                bits++;
                b >>= 1;
            }
            return bits;
        }
    }
    return 0;
}

/* RSA_PUBLIC_KEY (the blob starting with the "RSA1" magic). */
static void parse_rsa_public_key(const uint8_t* p, size_t n, rdp_mcs_info* info)
{
    uint32_t keylen;
    uint32_t bitlen;
    uint32_t exponent;
    size_t   modlen;
    size_t   i;
    size_t   skip;

    if (n < 20u)
        return;

    keylen   = get_le32(p + 4u);
    bitlen   = get_le32(p + 8u);
    exponent = get_le32(p + 16u);

    if (bitlen < 512u || bitlen > 2048u || (bitlen & 7u) != 0u)
        return;

    modlen = (size_t)bitlen / 8u;

    /* keylen counts the modulus plus eight bytes of zero padding. */
    if (keylen < modlen + 8u || modlen > n - 20u || modlen > sizeof(info->rsa_modulus))
        return;

    /* The proprietary certificate stores the modulus little-endian; the
       public API exposes it big-endian (as needed by the RSA code). */
    for (i = 0; i < modlen; i++)
        info->rsa_modulus[modlen - 1u - i] = p[20u + i];

    /* If the server stored the modulus big-endian instead, the reverse
       above would not have the advertised bit length while the raw field
       would.  Prefer the interpretation that matches bitlen. */
    if (modulus_bit_length(info->rsa_modulus, modlen) != (size_t)bitlen &&
        modulus_bit_length(p + 20u, modlen) == (size_t)bitlen)
    {
        memcpy(info->rsa_modulus, p + 20u, modlen);
    }

    skip = 0;
    while (skip + 1u < modlen && info->rsa_modulus[skip] == 0u)
        skip++;

    if (skip > 0u)
    {
        memmove(info->rsa_modulus, info->rsa_modulus + skip, modlen - skip);
        modlen -= skip;
    }

    exponent = normalize_exponent(exponent);
    if (exponent == 0u)
        return;

    info->rsa_bits     = (int)(modlen * 8u);
    info->rsa_exponent = exponent;
}

/* Parse the Server Security Data certificate.  Only the proprietary
   (RDP 4/5 style) certificate is understood; with Enhanced RDP Security
   this block carries no certificate at all. */
static void parse_certificate(const uint8_t* cert, size_t certlen,
                              rdp_mcs_info* info)
{
    size_t i;

    if (certlen < 4u)
        return;

    if ((get_le32(cert) & 0x7FFFFFFFu) != 1u)
        return;

    for (i = 0; i + 4u <= certlen; i++)
    {
        if (cert[i] == (uint8_t)'R' && cert[i + 1u] == (uint8_t)'S' &&
            cert[i + 2u] == (uint8_t)'A' && cert[i + 3u] == (uint8_t)'1')
        {
            parse_rsa_public_key(cert + i, certlen - i, info);
            return;
        }
    }
}

static int parse_server_core(const uint8_t* body, size_t len,
                             rdp_mcs_info* info)
{
    if (len < 8u)
        return -1;

    info->server_build = (int)get_le32(body); /* TS_UD_SC_CORE::version */

    return 0;
}

static int parse_server_network(const uint8_t* body, size_t len,
                                rdp_mcs_info* info)
{
    uint16_t io_id;
    uint16_t count;
    size_t   available;
    size_t   copy;
    size_t   i;

    if (len < 4u)
        return -1;

    io_id = get_le16(body);
    count = get_le16(body + 2u);

    info->io_channel = io_id;

    available = (len - 4u) / 2u;
    copy      = (size_t)count;
    if (copy > available)
        copy = available;
    if (copy > RDP_MCS_MAX_CHANNELS)
        copy = RDP_MCS_MAX_CHANNELS;

    for (i = 0; i < copy; i++)
        info->channels[i] = get_le16(body + 4u + i * 2u);

    info->channel_count = (int)copy;

    return 0;
}

static int parse_server_security(const uint8_t* body, size_t len,
                                 rdp_mcs_info* info)
{
    uint32_t method;
    uint32_t level;
    uint32_t random_len;
    uint32_t cert_len;

    if (len < 8u)
        return -1;

    method = get_le32(body);
    level  = get_le32(body + 4u);

    info->encryption_method = (int)method;
    info->encryption_level  = (int)level;

    if (method == 0u && level == 0u)
        return 0;                    /* no serverRandom / certificate */

    /* serverRandomLen and serverCertLen are both present when either the
       method or the level is non-zero. */
    if (len < 16u)
        return -1;

    random_len = get_le32(body + 8u);
    cert_len   = get_le32(body + 12u);

    if (random_len > len - 16u)
        return -1;
    if (cert_len > len - 16u - random_len)
        return -1;

    if (cert_len >= 4u)
        parse_certificate(body + 16u + random_len, cert_len, info);

    return 0;
}

static int parse_server_blocks(const uint8_t* p, size_t n, rdp_mcs_info* info)
{
    size_t off    = 0;
    int    blocks = 0;

    while (off < n)
    {
        uint16_t type;
        uint16_t blen;
        const uint8_t* body;
        size_t         body_len;
        int            rc = 0;

        if (n - off < 4u)
            return -1;

        type = get_le16(p + off);
        blen = get_le16(p + off + 2u);

        if (blen < 4u || (size_t)blen > n - off)
            return -1;

        body     = p + off + 4u;
        body_len = (size_t)blen - 4u;

        switch (type)
        {
            case SC_CORE:
                rc = parse_server_core(body, body_len, info);
                break;
            case SC_NET:
                rc = parse_server_network(body, body_len, info);
                break;
            case SC_SECURITY:
                rc = parse_server_security(body, body_len, info);
                break;
            default:
                break;
        }

        if (rc != 0)
            return -1;

        off += blen;
        blocks++;
    }

    return (blocks > 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* MCS Connect Response                                               */
/* ------------------------------------------------------------------ */

int rdp_mcs_parse_connect_response(const uint8_t* data, size_t len,
                                   rdp_mcs_info* info)
{
    mcs_rdr        r;
    mcs_rdr        app;
    size_t         app_len = 0;
    int            have_result = 0;
    const uint8_t* user_data = NULL;
    size_t         user_len = 0;

    if (data == NULL || info == NULL)
        return -1;

    memset(info, 0, sizeof(*info));

    if (len < 4u)
        return -1;

    r.p   = data;
    r.len = len;
    r.pos = 0;

    if (strip_transport_headers(&r) != 0)
        return -1;

    {
        uint8_t tag;
        if (rd_u8(&r, &tag) != 0 || tag != 0x7Fu)
            return -1;
        if (rd_u8(&r, &tag) != 0 || tag != 0x66u)
            return -1;
    }

    if (rd_ber_len(&r, &app_len) != 0)
        return -1;
    if (app_len > r.len - r.pos)
        return -1;

    app.p   = r.p + r.pos;
    app.len = app_len;
    app.pos = 0;

    /* Walk the ConnectResponse fields.  The usual order is result,
       calledConnectId, domainParameters, userData, but the data is located
       by tag so a different order does not matter. */
    while (app.pos < app.len)
    {
        uint8_t tag;
        size_t  clen;

        if (rd_u8(&app, &tag) != 0)
            return -1;
        if (tag == 0x00u)
            return -1;

        if (rd_ber_len(&app, &clen) != 0)
            return -1;
        if (clen > app.len - app.pos)
            return -1;

        switch (tag)
        {
            case 0x02u:              /* INTEGER */
            case 0x0Au:              /* ENUMERATED */
                if (!have_result)
                {
                    uint32_t value = 0;
                    size_t   i;

                    if (clen == 0 || clen > 4u)
                        return -1;

                    for (i = 0; i < clen; i++)
                        value = (value << 8) | app.p[app.pos + i];

                    if (value != 0)   /* ConnectResponse::result must be 0 */
                        return -1;

                    have_result = 1;
                }
                app.pos += clen;
                break;

            case 0x30u:              /* DomainParameters SEQUENCE */
                app.pos += clen;
                break;

            case 0x04u:              /* GCC Conference Create Response */
                if (clen > 0)
                {
                    user_data = app.p + app.pos;
                    user_len  = clen;
                }
                app.pos += clen;
                break;

            default:
                app.pos += clen;
                break;
        }
    }

    if (!have_result || user_data == NULL || user_len == 0)
        return -1;

    {
        const uint8_t* blocks = NULL;
        size_t         blocks_len = 0;
        rdp_mcs_info   parsed;

        memset(&parsed, 0, sizeof(parsed));

        if (gcc_find_blocks(user_data, user_len, &blocks, &blocks_len) == 0 &&
            parse_server_blocks(blocks, blocks_len, &parsed) == 0)
        {
            info->user_channel = 0;
            *info = parsed;
            return 0;
        }

        blocks     = NULL;
        blocks_len = 0;

        if (gcc_scan_blocks(user_data, user_len, &blocks, &blocks_len) == 0)
        {
            memset(&parsed, 0, sizeof(parsed));
            if (parse_server_blocks(blocks, blocks_len, &parsed) == 0)
            {
                info->user_channel = 0;
                *info = parsed;
                return 0;
            }
        }
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/* small MCS PDUs                                                     */
/* ------------------------------------------------------------------ */

int rdp_mcs_build_erect_domain(uint8_t* out, size_t outcap,
                               uint16_t user_channel)
{
    (void)user_channel; /* not used by Erect Domain Request */

    if (out == NULL || outcap < 12u)
        return -1;

    out[0]  = 0x03;
    out[1]  = 0x00;
    out[2]  = 0x00;
    out[3]  = 0x0C;
    out[4]  = 0x02;                   /* X.224 LI */
    out[5]  = 0xF0;                   /* Data TPDU */
    out[6]  = 0x80;                   /* EOT */
    out[7]  = 0x04;                   /* DomainMCSPDU: ErectDomainRequest */
    out[8]  = 0x01;                   /* subHeight INTEGER, length 1 */
    out[9]  = 0x00;
    out[10] = 0x01;                   /* subInterval INTEGER, length 1 */
    out[11] = 0x00;

    return 12;
}

int rdp_mcs_build_attach_user(uint8_t* out, size_t outcap)
{
    if (out == NULL || outcap < 8u)
        return -1;

    out[0] = 0x03;
    out[1] = 0x00;
    out[2] = 0x00;
    out[3] = 0x08;
    out[4] = 0x02;                    /* X.224 LI */
    out[5] = 0xF0;                    /* Data TPDU */
    out[6] = 0x80;                    /* EOT */
    out[7] = 0x28;                    /* DomainMCSPDU: AttachUserRequest */

    return 8;
}

static int parse_attach_confirm_fallback(const uint8_t* p, size_t n,
                                         uint16_t* user_channel)
{
    uint32_t vals[8];
    int      nv = 0;
    size_t   i;
    uint32_t raw;

    /* A BER ENUMERATED result, if present, must be success. */
    for (i = 0; i + 3u <= n; i++)
    {
        if (p[i] == 0x0Au && p[i + 1u] == 1u)
        {
            if (p[i + 2u] != 0u)
                return -1;
            break;
        }
    }

    /* Collect INTEGERs; the last one is the initiator (UserId - 1002). */
    i = 0;
    while (i + 2u <= n && nv < 8)
    {
        if (p[i] == 0x02u && p[i + 1u] >= 1u && p[i + 1u] <= 4u &&
            i + 2u + p[i + 1u] <= n)
        {
            size_t l = p[i + 1u];
            uint32_t v = 0;
            size_t k;

            for (k = 0; k < l; k++)
                v = (v << 8) | p[i + 2u + k];

            vals[nv++] = v;
            i += 2u + l;
        }
        else
        {
            i++;
        }
    }

    if (nv < 1)
        return -1;

    for (i = 0; i + 1u < (size_t)nv; i++)
    {
        if (vals[i] != 0u)
            return -1;
    }

    raw = vals[nv - 1];
    if (raw > 65535u - MCS_BASE_CHANNEL_ID)
        return -1;

    *user_channel = (uint16_t)(raw + MCS_BASE_CHANNEL_ID);
    return 0;
}

int rdp_mcs_parse_attach_user_confirm(const uint8_t* data, size_t len,
                                      uint16_t* user_channel)
{
    mcs_rdr r;

    if (data == NULL || user_channel == NULL || len < 3u)
        return -1;

    r.p   = data;
    r.len = len;
    r.pos = 0;

    if (strip_transport_headers(&r) != 0)
        return -1;

    if (r.len - r.pos >= 3u && (r.p[r.pos] >> 2) == 3u)
    {
        uint8_t result;
        uint16_t raw;

        r.pos++;                      /* DomainMCSPDU choice + options */

        if (rd_u8(&r, &result) != 0)
            return -1;

        if (result == 0x0Au)          /* BER ENUMERATED (defensive) */
        {
            size_t l = 0;
            if (rd_ber_len(&r, &l) != 0 || l != 1u)
                return -1;
            if (rd_u8(&r, &result) != 0)
                return -1;
        }

        if (result != 0u)
            return -1;

        if (r.len - r.pos < 2u)
            return -1;

        raw = get_be16(r.p + r.pos);
        if (raw > 65535u - MCS_BASE_CHANNEL_ID)
            return -1;

        *user_channel = (uint16_t)(raw + MCS_BASE_CHANNEL_ID);
        return 0;
    }

    return parse_attach_confirm_fallback(r.p + r.pos, r.len - r.pos,
                                         user_channel);
}

int rdp_mcs_build_channel_join(uint8_t* out, size_t outcap,
                               uint16_t user_channel, uint16_t channel_id)
{
    uint16_t raw;

    if (out == NULL || outcap < 12u)
        return -1;
    if (user_channel < MCS_BASE_CHANNEL_ID)
        return -1;

    raw = (uint16_t)(user_channel - MCS_BASE_CHANNEL_ID);

    out[0]  = 0x03;
    out[1]  = 0x00;
    out[2]  = 0x00;
    out[3]  = 0x0C;
    out[4]  = 0x02;                   /* X.224 LI */
    out[5]  = 0xF0;                   /* Data TPDU */
    out[6]  = 0x80;                   /* EOT */
    out[7]  = 0x38;                   /* DomainMCSPDU: ChannelJoinRequest */
    out[8]  = (uint8_t)(raw >> 8);
    out[9]  = (uint8_t)(raw & 0xFFu);
    out[10] = (uint8_t)(channel_id >> 8);
    out[11] = (uint8_t)(channel_id & 0xFFu);

    return 12;
}

/* ------------------------------------------------------------------ */
/* MCS SendData                                                       */
/* ------------------------------------------------------------------ */

int rdp_mcs_build_send_data(uint8_t* out, size_t outcap,
                            uint16_t user_channel, uint16_t channel_id,
                            const uint8_t* payload, size_t payload_len)
{
    size_t total;
    uint16_t raw;

    if (out == NULL || (payload == NULL && payload_len > 0))
        return -1;
    if (user_channel < MCS_BASE_CHANNEL_ID)
        return -1;
    if (payload_len > 0x7FFFu)
        return -1;

    total = MCS_SEND_DATA_HEADER + payload_len;
    if (total > 0xFFFFu || outcap < total)
        return -1;

    raw = (uint16_t)(user_channel - MCS_BASE_CHANNEL_ID);

    out[0]  = 0x03;
    out[1]  = 0x00;
    out[2]  = (uint8_t)(total >> 8);
    out[3]  = (uint8_t)(total & 0xFFu);
    out[4]  = 0x02;                   /* X.224 LI */
    out[5]  = 0xF0;                   /* Data TPDU */
    out[6]  = 0x80;                   /* EOT */
    out[7]  = (uint8_t)(MCS_PDU_SEND_DATA_REQ << 2); /* 0x64 */
    out[8]  = (uint8_t)(raw >> 8);
    out[9]  = (uint8_t)(raw & 0xFFu);
    out[10] = (uint8_t)(channel_id >> 8);
    out[11] = (uint8_t)(channel_id & 0xFFu);
    out[12] = 0x70;                   /* dataPriority + segmentation */
    out[13] = (uint8_t)(0x80u | (uint8_t)(payload_len >> 8));
    out[14] = (uint8_t)(payload_len & 0xFFu);

    if (payload_len > 0)
        memcpy(out + MCS_SEND_DATA_HEADER, payload, payload_len);

    return (int)total;
}

int rdp_mcs_extract_send_data(const uint8_t* tpkt_payload, size_t len,
                              const uint8_t** out, size_t* outlen)
{
    mcs_rdr r;
    uint8_t choice;
    size_t  data_len;

    if (tpkt_payload == NULL || out == NULL || outlen == NULL)
        return -1;

    *out    = NULL;
    *outlen = 0;

    if (len < 4u)
        return -1;

    r.p   = tpkt_payload;
    r.len = len;
    r.pos = 0;

    if (strip_transport_headers(&r) != 0)
        return -1;

    if (r.len - r.pos < 7u)           /* choice + initiator + channel + prio + len */
        return -1;

    choice = r.p[r.pos];
    if ((choice >> 2) != MCS_PDU_SEND_DATA_REQ &&
        (choice >> 2) != MCS_PDU_SEND_DATA_IND)
        return -1;

    r.pos++;                          /* DomainMCSPDU choice + options */

    if (rd_skip(&r, 2) != 0)          /* initiator (UserId) */
        return -1;
    if (rd_skip(&r, 2) != 0)          /* channelId */
        return -1;
    if (rd_skip(&r, 1) != 0)          /* dataPriority + segmentation */
        return -1;
    if (rd_per_len(&r, &data_len) != 0)
        return -1;
    if (data_len > r.len - r.pos)
        return -1;

    *out    = r.p + r.pos;
    *outlen = data_len;

    return 0;
}
