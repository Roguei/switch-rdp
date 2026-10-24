/*
 * rdp_session - RDP connection orchestration for switch_rdp.
 *
 *   TCP -> X.224 negotiation -> TLS (mbedTLS) -> CredSSP/NTLMv2 ->
 *   MCS connect/attach/join -> Client Info -> capability exchange ->
 *   update loop (bitmaps) + FastPath input.
 */

#ifdef __SWITCH__
#include <switch.h>          /* consoleUpdate() in nla_stage() */
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include "rdp_session.h"
#include "rdp_crypto.h"
#include "rdp_tls.h"
#include "rdp_nla.h"
#include "rdp_mcs.h"
#include "rdp_input.h"
#include "rdp_update.h"

#define RDP_PROTOCOL_RDP    0x00000000u
#define RDP_PROTOCOL_SSL    0x00000001u
#define RDP_PROTOCOL_HYBRID 0x00000002u

struct rdp_conn
{
    rdp_cfg      cfg;
    rdp_stage_fn stage;
    void*        stage_user;

    int          sock;
    rdp_tls_t*   tls;
    rdp_mcs_info mcs;

    rdp_surface  surface;
    uint8_t*     framebuffer;

    int          connected;
    uint32_t     share_id;
    int          caps_sent;
    int          use_sec_header;   /* prepend 4 zero bytes to share PDUs */
    int          wait_ms;          /* budget for the first byte of a PDU */
    int          mouse_x, mouse_y; /* last absolute pointer position */

    int          last_recv_err;
    uint8_t      last_hdr[8];

    /* receive buffer for the current PDU body */
    uint8_t      rx[65536];

    /* Fast-Path fragmented update reassembly.  A full-screen repaint arrives
     * as FIRST/NEXT/LAST fragments, so this must hold a whole logical update
     * (a 1024x768 32bpp bitmap update is ~47 KiB on the wire). */
    uint8_t*     frag;
    size_t       frag_len;
    int          frag_active;

    /* statistics / change tracking */
    unsigned     updates;
    unsigned     rects;
    int          dirty;
};

/* Whole-logical-update reassembly buffer (see the frag comment in rdp_conn). */
#define RDP_FRAG_CAP (1024u * 1024u)

#define LOG(c, ...) do { if ((c)->stage) { char m[220]; snprintf(m, sizeof(m), __VA_ARGS__); (c)->stage(m, (c)->stage_user); } } while (0)

/* ------------------------------------------------------------------ */
/* transport                                                          */
/* ------------------------------------------------------------------ */
static int wait_fd(int fd, short events, int timeout_ms)
{
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    int rc = poll(&pfd, 1, timeout_ms);
    if (rc <= 0) return -1;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    return 0;
}

static int tr_send(rdp_conn* c, const void* buf, size_t len)
{
    if (c->tls)
        return rdp_tls_write(c->tls, buf, len) < 0 ? -1 : (int)len;

    const uint8_t* p = (const uint8_t*)buf;
    size_t done = 0;
    while (done < len)
    {
        int n = (int)send(c->sock, p + done, len - done, 0);
        if (n < 0)
        {
            if (wait_fd(c->sock, POLLOUT, 5000) != 0) return -1;
            continue;
        }
        if (n == 0) return -1;
        done += (size_t)n;
    }
    return (int)len;
}

static int tr_recv_exact(rdp_conn* c, void* buf, size_t len)
{
    uint8_t* p = (uint8_t*)buf;
    size_t got = 0;
    while (got < len)
    {
        int n;
        if (c->tls)
        {
            n = rdp_tls_read(c->tls, p + got, len - got);
            if (n == RDP_TLS_AGAIN)
            {
                /* only legal before a single byte has been consumed */
                if (got == 0) return -2;
                c->last_recv_err = n;
                return -1;
            }
            if (n < 0) c->last_recv_err = n;
        }
        else
        {
            if (wait_fd(c->sock, POLLIN, 8000) != 0)
            {
                c->last_recv_err = -2;
                return -1;
            }
            n = (int)recv(c->sock, p + got, len - got, 0);
            if (n == 0) { c->last_recv_err = -1; return -1; }
            if (n < 0)  { c->last_recv_err = errno; return -1; }
        }
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

static int tr_recv_tpkt(rdp_conn* c, uint8_t* out, size_t cap)
{
    uint8_t hdr[4];
    if (tr_recv_exact(c, hdr, 4) != 0) return -1;
    c->last_hdr[0] = hdr[0]; c->last_hdr[1] = hdr[1];
    c->last_hdr[2] = hdr[2]; c->last_hdr[3] = hdr[3];
    if (hdr[0] != 0x03) return -1;
    size_t total = ((size_t)hdr[2] << 8) | hdr[3];
    if (total < 4 || total - 4 > cap) return -1;
    size_t body = total - 4;
    if (body && tr_recv_exact(c, out, body) != 0) return -1;
    return (int)body;
}

/* ------------------------------------------------------------------ */
/* X.224                                                              */
/* ------------------------------------------------------------------ */
static int x224_connect(rdp_conn* c, uint32_t* selected)
{
    uint8_t req[24];
    size_t n = 0;
    req[n++] = 0x03; req[n++] = 0x00; req[n++] = 0x00; req[n++] = 0x00;
    req[n++] = 0x0E;                       /* LI, fixed below */
    req[n++] = 0xE0;                       /* CR TPDU */
    req[n++] = 0x00; req[n++] = 0x00;      /* dst-ref */
    req[n++] = 0x00; req[n++] = 0x00;      /* src-ref */
    req[n++] = 0x00;                       /* class */
    req[n++] = 0x01; req[n++] = 0x00;      /* RDP_NEG_REQ, flags */
    req[n++] = 0x08; req[n++] = 0x00;      /* length = 8 */
    /* Servers (Windows and xrdp) usually require NLA, so offer HYBRID first */
    /* 0x0B = HYBRID_EX | HYBRID | SSL | RDP, exactly what mstsc requests */
    uint32_t rq = 0x0Bu;
    req[n++] = (uint8_t)rq; req[n++] = (uint8_t)(rq >> 8);
    req[n++] = (uint8_t)(rq >> 16); req[n++] = (uint8_t)(rq >> 24);

    req[2] = (uint8_t)(n >> 8); req[3] = (uint8_t)n;
    req[4] = (uint8_t)(n - 5);

    LOG(c, "X.224 tx %d: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
        (int)n, req[0], req[1], req[2], req[3], req[4], req[5], req[6], req[7],
        req[8], req[9], req[10], req[11], req[12], req[13], req[14], req[15],
        req[16], req[17], req[18]);

    if (tr_send(c, req, n) != (int)n) { LOG(c, "X.224 send failed"); return -1; }

    uint8_t raw[64];
    memset(raw, 0, sizeof(raw));
    int got;
    if (!c->tls)
    {
        if (wait_fd(c->sock, POLLIN, 8000) != 0) { LOG(c, "X.224: no reply (timeout)"); return -1; }
        got = (int)recv(c->sock, raw, sizeof(raw), 0);
    }
    else
    {
        got = rdp_tls_read(c->tls, raw, sizeof(raw));
    }
    if (got <= 0) { LOG(c, "X.224 recv failed (%d, errno %d)", got, errno); return -1; }

    LOG(c, "X.224 rx %d: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
        got, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
        raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14], raw[15]);

    memcpy(c->last_hdr, raw, 8);

    *selected = RDP_PROTOCOL_RDP;
    if (got < 11 || raw[0] != 0x03 || (raw[5] != 0xD0 && raw[5] != 0xE0))
    {
        LOG(c, "X.224: unexpected reply");
        return -1;
    }

    size_t off = 11;
    if (got >= (int)(off + 8) && (raw[off] == 0x02 || raw[off] == 0x03))
    {
        *selected = (uint32_t)raw[off+4] | ((uint32_t)raw[off+5] << 8) |
                    ((uint32_t)raw[off+6] << 16) | ((uint32_t)raw[off+7] << 24);
        if (raw[off] == 0x03)
        {
            LOG(c, "server refused negotiation");
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* client info                                                        */
/* ------------------------------------------------------------------ */
static int send_client_info(rdp_conn* c)
{
    const char* user = c->cfg.user ? c->cfg.user : "";
    const char* pass = c->cfg.pass ? c->cfg.pass : "";
    const char* dom  = c->cfg.domain ? c->cfg.domain : "";

    uint8_t info[1024];
    size_t n = 0;
    size_t ulen = strlen(user) * 2;
    size_t plen = strlen(pass) * 2;
    size_t dlen = strlen(dom) * 2;
    const char* strs[5];
    size_t lens[5];
    int i;

    strs[0] = dom;  lens[0] = dlen;
    strs[1] = user; lens[1] = ulen;
    strs[2] = pass; lens[2] = plen;
    strs[3] = "";   lens[3] = 0;
    strs[4] = "";   lens[4] = 0;

    info[n++] = 0x09; info[n++] = 0x04;               /* codePage 0x0409 */
    info[n++] = 0; info[n++] = 0;
    info[n++] = 0x10; info[n++] = 0;                  /* flags = INFO_UNICODE */
    info[n++] = 0; info[n++] = 0;
    for (i = 0; i < 5; i++)
    {
        info[n++] = (uint8_t)(lens[i] & 0xFF);
        info[n++] = (uint8_t)(lens[i] >> 8);
    }
    for (i = 0; i < 5; i++)
    {
        size_t k;
        for (k = 0; k < lens[i]; k += 2)
        {
            info[n++] = (uint8_t)strs[i][k / 2];
            info[n++] = 0;
        }
        info[n++] = 0; info[n++] = 0;                 /* null terminator */
    }
    /* TS_EXTENDED_INFO: clientAddressFamily + cbClientAddress + address,
     * then cbClientDir + dir.  Empty strings, but the fields must be there. */
    info[n++] = 2; info[n++] = 0;
    info[n++] = 2; info[n++] = 0;
    info[n++] = 0; info[n++] = 0;
    info[n++] = 2; info[n++] = 0;
    info[n++] = 0; info[n++] = 0;

    {
        /* The Client Info PDU is special: it carries the 4-byte security
         * header (SEC_INFO_PKT) directly, with no share-data header. */
        uint8_t pkt[1100];
        uint8_t out[1200];
        size_t k = 0;
        int w;
        pkt[k++] = 0x40; pkt[k++] = 0x00;
        pkt[k++] = 0x00; pkt[k++] = 0x00;
        memcpy(pkt + k, info, n);
        k += n;
        w = rdp_mcs_build_send_data(out, sizeof(out), c->mcs.user_channel,
                                    c->mcs.io_channel, pkt, k);
        if (w <= 0) return -1;
        if (getenv("RDP_DUMP"))
        {
            char line[256];
            int j, o = 0;
            line[0] = 0;
            for (j = (int)k > 64 ? (int)k - 16 : 0; j < (int)k && o < 240; j++)
                o += sprintf(line + o, "%02x", pkt[j]);
            LOG(c, "client info %d bytes (mcs %d) tail: %s", (int)k, w, line);
        }
        return tr_send(c, out, (size_t)w) < 0 ? -1 : 0;
    }
}

/* ------------------------------------------------------------------ */
/* connect                                                            */
/* ------------------------------------------------------------------ */
rdp_conn* rdp_conn_new(const rdp_cfg* cfg, rdp_stage_fn stage, void* user)
{
    rdp_conn* c = (rdp_conn*)calloc(1, sizeof(rdp_conn));
    if (!c) return NULL;
    c->cfg = *cfg;
    c->stage = stage;
    c->stage_user = user;
    c->sock = -1;

    c->frag = (uint8_t*)malloc(RDP_FRAG_CAP);
    c->framebuffer = (uint8_t*)malloc((size_t)cfg->width * (size_t)cfg->height * 4);
    if (!c->framebuffer) { free(c); return NULL; }
    memset(c->framebuffer, 0x20, (size_t)cfg->width * (size_t)cfg->height * 4);
    c->surface.pixels = c->framebuffer;
    c->surface.width  = cfg->width;
    c->surface.height = cfg->height;
    return c;
}

static int tcp_connect(rdp_conn* c)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    int one = 1;
    int rc;
    int waited = 0;
    int fl;

    c->sock = -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", c->cfg.port);
    rc = getaddrinfo(c->cfg.host, portstr, &hints, &res);
    if (rc != 0 || !res)
    {
        LOG(c, "getaddrinfo(%s) failed %d", c->cfg.host, rc);
        return RDP_ERR_ADDRESS;
    }

    for (ai = res; ai; ai = ai->ai_next)
    {
        int so = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        int err = 0;
        socklen_t elen = sizeof(err);
        if (so < 0) continue;

        /* Non-blocking connect: the previous build did a blocking connect(),
         * which on a wrong IP sat in SYN retries for ~2 minutes with the UI
         * frozen and no way to cancel. */
        fl = fcntl(so, F_GETFL, 0);
        if (fl >= 0) fcntl(so, F_SETFL, fl | O_NONBLOCK);
        setsockopt(so, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        rc = connect(so, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno != EINPROGRESS)
        {
            int e = errno;
            close(so);
            LOG(c, "connect() immediate error %d", e);
            if (e == ECONNREFUSED) { freeaddrinfo(res); return RDP_ERR_REFUSED; }
            if (e == EHOSTUNREACH || e == ENETUNREACH) { freeaddrinfo(res); return RDP_ERR_UNREACH; }
            continue;
        }
        if (rc != 0)
        {
            struct pollfd pfd;
            pfd.fd = so;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            waited = 0;
            for (;;)
            {
                int pr = poll(&pfd, 1, 200);
                if (pr > 0) break;
                waited += 200;
                if (rdp_cancel_hook && rdp_cancel_hook())
                {
                    LOG(c, "connect cancelled by user");
                    close(so);
                    freeaddrinfo(res);
                    return RDP_ERR_CANCELLED;
                }
                if (waited >= 6000)
                {
                    LOG(c, "connect timeout");
                    close(so);
                    freeaddrinfo(res);
                    return RDP_ERR_NETWORK;
                }
            }
            if (getsockopt(so, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0)
            {
                LOG(c, "connect failed after poll, so_error=%d", err);
                close(so);
                if (err == ECONNREFUSED) { freeaddrinfo(res); return RDP_ERR_REFUSED; }
                if (err == EHOSTUNREACH || err == ENETUNREACH || err == ETIMEDOUT)
                { freeaddrinfo(res); return RDP_ERR_UNREACH; }
                continue;
            }
        }
        c->sock = so;
        break;
    }
    freeaddrinfo(res);
    if (c->sock < 0) return RDP_ERR_NETWORK;

    return 0;
}

/* Cleared by main() right before consoleExit(): after that point the libnx
 * console renderer has released its framebuffer, so any printf/consoleUpdate
 * from a late LOG() line dereferences NULL and takes the process down. */
int rdp_console_live = 0;

/* The UI has no libnx console: it draws everything itself.  These two hooks let
 * the session layer report progress and be cancelled from the pad while it is
 * blocked waiting for the network. */
void (*rdp_ui_stage_hook)(const char* text) = 0;
int  (*rdp_cancel_hook)(void) = 0;

static void nla_stage(const char* msg)
{
    if (rdp_ui_stage_hook) rdp_ui_stage_hook(msg);
    if (!rdp_console_live) return;
    printf("  %s\n", msg);
#ifdef __SWITCH__
    consoleUpdate(NULL);
#endif
}

int rdp_conn_connect(rdp_conn* c)
{
    nla_log = nla_stage;
    uint32_t selected = RDP_PROTOCOL_RDP;
    int rc = 0;

    LOG(c, "TCP connecting to %s:%d", c->cfg.host, c->cfg.port);
    rc = tcp_connect(c);
    if (rc != 0) { LOG(c, "TCP connect failed (%d)", rc); return rc; }
    LOG(c, "TCP connected");

    if (x224_connect(c, &selected) != 0) { LOG(c, "X.224 negotiation failed"); return RDP_ERR_X224; }
    rdp_mcs_set_selected_protocol(selected);

    if (selected == RDP_PROTOCOL_HYBRID)      LOG(c, "X.224: NLA (TLS+CredSSP)");
    else if (selected == 0x08)                LOG(c, "X.224: NLA extended (HYBRID_EX)");
    else if (selected == RDP_PROTOCOL_SSL)    LOG(c, "X.224: TLS");
    else                                      LOG(c, "X.224: standard RDP security");

    if (selected == RDP_PROTOCOL_HYBRID || selected == RDP_PROTOCOL_SSL || selected == 0x08)
    {
        LOG(c, "TLS: creating context");
        c->tls = rdp_tls_new(c->sock, c->cfg.host);
        if (!c->tls) { LOG(c, "TLS context failed"); return -1; }
        LOG(c, "TLS: context ok, handshake start");
        rc = rdp_tls_handshake(c->tls);
        if (rc != 0) { LOG(c, "TLS handshake failed (%d)", rc); return RDP_ERR_TLS; }
        LOG(c, "TLS established");
    }

    if (selected == RDP_PROTOCOL_HYBRID || selected == 0x08)
    {
        nla_cred cred;
        memset(&cred, 0, sizeof(cred));
        cred.user = c->cfg.user ? c->cfg.user : "";
        cred.pass = c->cfg.pass ? c->cfg.pass : "";
        cred.domain = c->cfg.domain ? c->cfg.domain : "";
        LOG(c, "NLA authentication...");
        int nla_rc = nla_authenticate(c->tls, &cred, NULL);
        LOG(c, "NLA result %d", nla_rc);
        if (nla_rc != 0)
        {
            /* wrong password / no remote desktop right: report it instead of
             * limping into MCS and dying with a generic disconnect */
            LOG(c, "NLA authentication rejected");
            return RDP_ERR_AUTH;
        }
        LOG(c, "NLA authenticated");
    }

    {
        static const char names[4][8] = { "rdpdr\0\0\0", "cliprdr", "rdpsnd\0\0", "drdynvc" };
        uint8_t pdu[1024];
        int n = rdp_mcs_build_connect_initial(pdu, sizeof(pdu),
                                              c->cfg.width, c->cfg.height, 32,
                                              (const char*)names, 4, "SWITCH");
        if (n <= 0) { LOG(c, "MCS connect-initial failed"); return RDP_ERR_MCS; }
        {
            int sent = tr_send(c, pdu, (size_t)n);
            LOG(c, "MCS connect-initial %d bytes, sent %d", n, sent);
            if (sent < 0) { LOG(c, "MCS send failed"); return RDP_ERR_MCS; }
        }
    }

    {
        uint8_t rsp[8192];
        int len = tr_recv_tpkt(c, rsp, sizeof(rsp));
        if (len < 0) { LOG(c, "MCS response read failed (err=%d, hdr=%02X %02X %02X %02X)",
                           c->last_recv_err, c->last_hdr[0], c->last_hdr[1],
                           c->last_hdr[2], c->last_hdr[3]); return RDP_ERR_MCS; }
        if (rdp_mcs_parse_connect_response(rsp, (size_t)len, &c->mcs) != 0)
        {
            LOG(c, "MCS response parse failed");
            return RDP_ERR_MCS;
        }
        LOG(c, "connect response %d bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
            len, rsp[0], rsp[1], rsp[2], rsp[3], rsp[4], rsp[5], rsp[6], rsp[7]);
        if (getenv("RDP_DUMP") && len > 0)
        {
            int k;
            char line[256];
            for (k = 0; k < len; k += 24)
            {
                int j, o = 0;
                line[0] = 0;
                for (j = k; j < k + 24 && j < len; j++)
                    o += sprintf(line + o, "%02x", rsp[j]);
                LOG(c, "  %03d: %s", k, line);
            }
        }
        LOG(c, "MCS: io=%u ch=%d rsa=%d", c->mcs.io_channel,
            c->mcs.channel_count, c->mcs.rsa_bits);
    }

    {
        uint8_t pdu[64];
        uint8_t rsp[512];
        int n, len;

        n = rdp_mcs_build_erect_domain(pdu, sizeof(pdu), c->mcs.io_channel);
        if (n > 0) tr_send(c, pdu, (size_t)n);

        n = rdp_mcs_build_attach_user(pdu, sizeof(pdu));
        if (n <= 0 || tr_send(c, pdu, (size_t)n) < 0) { LOG(c, "attach user failed"); return -1; }

        len = tr_recv_tpkt(c, rsp, sizeof(rsp));
        if (len < 4) { LOG(c, "attach user confirm failed (%d)", len); return -1; }
        if (getenv("RDP_DUMP"))
        {
            int j, o = 0;
            char line[256];
            line[0] = 0;
            for (j = 0; j < len && o < 240; j++) o += sprintf(line + o, "%02x", rsp[j]);
            LOG(c, "attach confirm %d bytes: %s", len, line);
        }
        if (rdp_mcs_parse_attach_user_confirm(rsp, (size_t)len,
                                              &c->mcs.user_channel) != 0)
        {
            /* Fallback: the responder's UserId is the last two bytes. */
            c->mcs.user_channel =
                (uint16_t)(((rsp[len - 2] << 8) | rsp[len - 1]) + 1001);
        }
        LOG(c, "MCS attached, user channel %u (initiator %u, reply %d bytes)",
            c->mcs.user_channel,
            (unsigned)(c->mcs.user_channel - 1001), len);

        /* Channel Join: one request/reply pair per channel, exactly like a real
         * client.  The first channel joined is the client's own user channel. */
        {
            int i;
            int total = c->mcs.channel_count + 2;
            for (i = 0; i < total; i++)
            {
                uint16_t target;
                if (i == 0) target = c->mcs.user_channel;
                else if (i == total - 1) target = c->mcs.io_channel;
                else target = (uint16_t)c->mcs.channels[i - 1];

                n = rdp_mcs_build_channel_join(pdu, sizeof(pdu),
                                               c->mcs.user_channel, target);
                if (n <= 0 || tr_send(c, pdu, (size_t)n) < 0) break;
                len = tr_recv_tpkt(c, rsp, sizeof(rsp));
                if (len < 4)
                {
                    LOG(c, "channel join %u failed (%d)", target, len);
                    return -1;
                }
                if (getenv("RDP_DUMP"))
                {
                    int j, o = 0;
                    char line[256];
                    line[0] = 0;
                    for (j = 0; j < len && o < 200; j++) o += sprintf(line + o, "%02x", rsp[j]);
                    LOG(c, "  join %u (init %u) reply %d: %s", target,
                        (unsigned)(c->mcs.user_channel - 1001), len, line);
                }
            }
            LOG(c, "channels joined (%d)", total);
        }
    }


    if (send_client_info(c) != 0) { LOG(c, "client info failed"); return -1; }
    LOG(c, "client info sent");

    c->connected = 1;
    return 0;
}

rdp_surface* rdp_conn_surface(rdp_conn* c) { return &c->surface; }

/* ------------------------------------------------------------------ */
/* updates                                                            */
/* ------------------------------------------------------------------ */
static void send_confirm_active(rdp_conn* c, const uint8_t* demand, size_t len)
{
    /* The server's Demand Active layout (verified on a live Windows host):
     *   totalLength(2) pduType(2) pduSource(2) shareId(4)
     *   lengthSourceDescriptor(2) lengthCombinedCapabilities(2)
     *   sourceDescriptor[lsd] numberCapabilities(2) pad2Octets(2)
     *   capabilitySets[lengthCombinedCapabilities-4]
     *
     * Windows accepts the echoed capability sets verbatim, with one important
     * exception: CAPSTYPE_BITMAP_CODECS (0x001D) must be dropped, otherwise the
     * server renders the desktop with a bitmap codec we do not implement.
     */
    static uint8_t caps[4096];
    size_t ncaps = 0;
    size_t s, i;
    int saw_input_cap = 0;

    if (len < 18) return;
    c->share_id = (uint32_t)demand[6] | ((uint32_t)demand[7] << 8) |
                  ((uint32_t)demand[8] << 16) | ((uint32_t)demand[9] << 24);
    {
        /* TS_DEMAND_ACTIVE_PDU:
         *   shareId(4) lengthSourceDescriptor(2) lengthCombinedCapabilities(2)
         *   sourceDescriptor[lsd] numberCapabilities(2) pad2Octets(2) capsets
         * Note: no originatorId here - that field only exists in Confirm Active. */
        uint16_t lsd = (uint16_t)(demand[10] | (demand[11] << 8));
        uint16_t lcc = (uint16_t)(demand[12] | (demand[13] << 8));
        size_t cap_off = 18u + (size_t)lsd;
        size_t cap_end = cap_off + ((lcc >= 4) ? (size_t)(lcc - 4) : 0u);
        size_t ncap = (uint16_t)(demand[14u + lsd] | (demand[15u + lsd] << 8));

        if (cap_off > len) return;
        if (cap_end > len) cap_end = len;

        s = cap_off;
        for (i = 0; i < ncap && s + 4 <= cap_end; i++)
        {
            uint16_t ctype = (uint16_t)(demand[s] | (demand[s + 1] << 8));
            uint16_t clen = (uint16_t)(demand[s + 2] | (demand[s + 3] << 8));
            if (clen < 4 || s + clen > cap_end) break;
            if (getenv("RDP_DUMP"))
            {
                int q, oo = 0; char ln2[80]; ln2[0] = 0;
                for (q = 0; q < 12 && q < (int)clen; q++)
                    oo += sprintf(ln2 + oo, "%02x", demand[s + q]);
                LOG(c, "  srv capset 0x%04X len=%u: %s", (unsigned)ctype,
                    (unsigned)clen, ln2);
            }
            /* capset inventory: the "black screen on Server 2012" log needs to
         * show what the server offered and what we did with it. */
        LOG(c, "caps server 0x%04X len=%u -> %s", (unsigned)ctype, (unsigned)clen,
            ctype == 0x001D ? "dropped(bitmap codecs)" : "echoed");
        if (ctype == 0x001D && rdp_codec_mode == 1)
        {
            /* present but empty: 4-byte header + count(0) + pad(0) */
            uint8_t empty[8] = { 0x1D, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00 };
            if (ncaps + 8 <= sizeof(caps)) { memcpy(caps + ncaps, empty, 8); ncaps += 8; }
            LOG(c, "caps server 0x%04X len=%u -> returned empty", (unsigned)ctype, (unsigned)clen);
        }
        if (ctype != 0x001D && ncaps + clen <= sizeof(caps))
            {
                memcpy(caps + ncaps, demand + s, clen);
                if (ctype == 0x000D)
                {
                    /* TS_INPUT_CAPABILITYSET (88 bytes on this protocol
                     * version).  The server's own flags already enable
                     * scancode and Fast-Path input; make sure of it anyway. */
                    saw_input_cap = 1;
                    if (clen >= 6)
                    {
                        uint16_t in_flags = (uint16_t)(caps[ncaps + 4] |
                                                       (caps[ncaps + 5] << 8));
                        in_flags |= 0x0001u;   /* INPUT_FLAG_SCANCODES      */
                        in_flags |= 0x0008u;   /* INPUT_FLAG_FASTPATH_INPUT */
                        in_flags |= 0x0020u;   /* INPUT_FLAG_FASTPATH_INPUT2*/
                        caps[ncaps + 4] = (uint8_t)(in_flags & 0xFFu);
                        caps[ncaps + 5] = (uint8_t)(in_flags >> 8);
                    }
                }
                if (ctype == 0x0003 && clen == 88 && !getenv("RDP_ORDERS"))
                {
                    /* TS_ORDER_CAPABILITYSET.  This client paints bitmaps and
                     * nothing else, so it must not claim any drawing order:
                     * with orderSupport[] advertised, Windows renders every
                     * window with orders (fastpath update code 0) and the
                     * desktop comes out smeared.  Zeroing orderSupport (while
                     * keeping orderFlags intact - clearing that makes the
                     * server drop the session with error 0x10EA) makes the
                     * server send plain bitmap updates instead. */
                    memset(caps + ncaps + 36, 0, 32);   /* orderSupport[32] */
                    memset(caps + ncaps + 70, 0, 2);    /* orderSupportExFlg*/
                }
                ncaps += clen;
            }
            s += clen;
        }
    }

    if (!saw_input_cap && ncaps + 8 <= sizeof(caps))
    {
        /* The server sends no TS_INPUT_CAPABILITYSET, and echoing only what it
         * sent would advertise "this client cannot type".  Append one, with
         * scancode + Fast-Path input, so the session accepts keyboard/mouse. */
        caps[ncaps + 0] = 0x0D; caps[ncaps + 1] = 0x00;
        caps[ncaps + 2] = 0x0C; caps[ncaps + 3] = 0x00;   /* 12-byte capset */
        caps[ncaps + 4] = 0x29; caps[ncaps + 5] = 0x00;   /* SCANCODES|FP|FP2 */
        caps[ncaps + 6] = 0x00; caps[ncaps + 7] = 0x00;
        caps[ncaps + 8] = 0x00; caps[ncaps + 9] = 0x00;
        caps[ncaps + 10] = 0x00; caps[ncaps + 11] = 0x00;
        ncaps += 12;
        LOG(c, "confirm active: appended input capset");
    }

    {
        uint8_t out[4600];
        size_t n = 0;
        const char* src = "RDP";

        out[n++] = 0; out[n++] = 0;                       /* totalLength */
        out[n++] = 0x13; out[n++] = 0x00;                 /* pduType */
        out[n++] = (uint8_t)c->mcs.user_channel;          /* pduSource */
        out[n++] = (uint8_t)(c->mcs.user_channel >> 8);
        out[n++] = (uint8_t)c->share_id; out[n++] = (uint8_t)(c->share_id >> 8);
        out[n++] = (uint8_t)(c->share_id >> 16); out[n++] = (uint8_t)(c->share_id >> 24);
        out[n++] = 0xEA; out[n++] = 0x03;                 /* originatorId */
        out[n++] = 4; out[n++] = 0;                       /* lengthSourceDescriptor */
        out[n++] = (uint8_t)((ncaps + 4) & 0xFF);         /* lengthCombinedCapabilities */
        out[n++] = (uint8_t)((ncaps + 4) >> 8);
        out[n++] = (uint8_t)src[0]; out[n++] = (uint8_t)src[1];
        out[n++] = (uint8_t)src[2]; out[n++] = 0;
        /* count the cap sets we are about to append */
        {
            uint16_t cnt = 0;
            size_t k = 0;
            while (k + 4 <= ncaps)
            {
                uint16_t cl = (uint16_t)(caps[k + 2] | (caps[k + 3] << 8));
                if (cl < 4) break;
                cnt++;
                k += cl;
            }
            out[n++] = (uint8_t)(cnt & 0xFF);
            out[n++] = (uint8_t)(cnt >> 8);
        }
        out[n++] = 0; out[n++] = 0;                       /* pad2Octets */
        memcpy(out + n, caps, ncaps);
        n += ncaps;
        out[0] = (uint8_t)(n & 0xFF);
        out[1] = (uint8_t)(n >> 8);

        {
            uint8_t pdu[4800];
            int w = rdp_mcs_build_send_data(pdu, sizeof(pdu), c->mcs.user_channel,
                                            c->mcs.io_channel, out, n);
            if (w > 0) tr_send(c, pdu, (size_t)w);
        }
        LOG(c, "confirm active: %u capset bytes, %u total", (unsigned)ncaps, (unsigned)n);
    }
}

/* One slow-path share data PDU (client -> server).
 *   totalLength pduType(0x0017) pduSource shareId pad1 streamId
 *   uncompressedLength pduType2 compressedType compressedLength body
 * uncompressedLength is the length of the *body*, not of the whole PDU.
 */
static int send_data_pdu(rdp_conn* c, uint8_t pdu2, const uint8_t* body, size_t blen)
{
    uint8_t out[1024];
    uint8_t pdu[1200];
    size_t n = 0;
    int w;

    if (18 + blen > sizeof(out)) return -1;
    out[n++] = 0; out[n++] = 0;
    out[n++] = 0x17; out[n++] = 0x00;
    out[n++] = (uint8_t)c->mcs.user_channel;
    out[n++] = (uint8_t)(c->mcs.user_channel >> 8);
    out[n++] = (uint8_t)c->share_id; out[n++] = (uint8_t)(c->share_id >> 8);
    out[n++] = (uint8_t)(c->share_id >> 16); out[n++] = (uint8_t)(c->share_id >> 24);
    out[n++] = 0;                                     /* pad1 */
    out[n++] = 1;                                     /* streamId */
    /* uncompressedLength counts the whole share data PDU (header included):
     * the server's own data PDUs carry totalLength here (verified on a live
     * Windows host: total=22, body=4, uncompressedLength=22).  Sending the
     * body length instead makes the server drop every client data PDU, which
     * silently kills finalization and all keyboard/mouse input. */
    out[n++] = (uint8_t)((blen + 18) & 0xFF);         /* uncompressedLength */
    out[n++] = (uint8_t)((blen + 18) >> 8);
    out[n++] = pdu2;                                  /* pduType2 */
    out[n++] = 0;                                     /* compressedType */
    out[n++] = 0; out[n++] = 0;                       /* compressedLength */
    memcpy(out + n, body, blen);
    n += blen;
    if (getenv("RDP_DUMP2"))
    {
        static int shown = 0;
        if (shown < 8)
        {
            size_t k; int o = 0; char line[120]; line[0] = 0;
            shown++;
            for (k = 18; k < n && k < 34 && o < 110; k++)
                o += sprintf(line + o, "%02x", out[k]);
            LOG(c, "CLI body type2=0x%02X total=%u: %s", pdu2, (unsigned)n, line);
        }
    }
    out[0] = (uint8_t)(n & 0xFF);
    out[1] = (uint8_t)(n >> 8);

    w = rdp_mcs_build_send_data(pdu, sizeof(pdu), c->mcs.user_channel,
                                c->mcs.io_channel, out, n);
    if (w <= 0) return -1;
    return tr_send(c, pdu, (size_t)w) < 0 ? -1 : 0;
}

static void send_finalization(rdp_conn* c)
{
    uint8_t body[64];

    /* Client Synchronize */
    body[0] = 1; body[1] = 0; body[2] = 0; body[3] = 0;
    LOG(c, "tx activation pdu 0x0x1F");
    send_data_pdu(c, 0x1F, body, 4);

    /* Client Control: Cooperate */
    body[0] = 4; body[1] = 0;
    body[2] = 0; body[3] = 0;
    body[4] = 0; body[5] = 0; body[6] = 0; body[7] = 0;
    LOG(c, "tx activation pdu 0x0x14");
    send_data_pdu(c, 0x14, body, 8);

    /* Client Control: Request Control */
    body[0] = 1; body[1] = 0;
    LOG(c, "tx activation pdu 0x0x14");
    send_data_pdu(c, 0x14, body, 8);

    /* Client Persistent Key List */
    memset(body, 0, 24);
    LOG(c, "tx activation pdu 0x0x2B");
    send_data_pdu(c, 0x2B, body, 24);

    /* Client Font List (0x27; 0x28 is the server-only Font Map) */
    body[0] = 0; body[1] = 0;
    body[2] = 0; body[3] = 0;
    body[4] = 3; body[5] = 0;
    body[6] = 0x32; body[7] = 0;
    LOG(c, "tx activation pdu 0x0x27");
    send_data_pdu(c, 0x27, body, 8);

    c->caps_sent = 1;
    LOG(c, "finalization sent");
}

static int handle_update(rdp_conn* c, const uint8_t* p, size_t len)
{
    int n = rdp_update_bitmap(p, len, &c->surface);
    if (n > 0)
    {
        c->rects += (unsigned)n;
        c->dirty = 1;
    }
    return n > 0 ? 1 : 0;
}

/* Reads one PDU, transparently handling TPKT (slow path) and Fast-Path
 * framing.  Returns 1 on success, -1 on connection error.
 *   *is_fast = 1 -> (data,len) is a Fast-Path update payload
 *   *is_fast = 0, *chan = 0 -> raw bytes that were not an MCS PDU
 *   *is_fast = 0, *chan != 0 -> MCS SendData indication data
 */
/* How to answer the server's bitmap-codecs capset (0x001D):
 *   0 = drop it entirely (default, what Windows 10 is happy with)
 *   1 = send it back with zero codecs (some servers then fall back to raw)
 *   2 = echo the server's list untouched
 * Selected at runtime from the config file ("codecs = empty|keep"). */
int rdp_codec_mode = 0;

/* pdu-log: record the first few hundred PDUs so a "black screen" log shows
 * exactly what the server sent (channel, share pduType, length). */
static int g_pdu_log = 0;

static int recv_pdu(rdp_conn* c, uint16_t* chan, const uint8_t** data,
                    size_t* dlen, int* is_fast)
{
    uint8_t h0;

    *chan = 0;
    *data = NULL;
    *dlen = 0;
    *is_fast = 0;

    if (c->tls) rdp_tls_set_timeout(c->tls, c->wait_ms);
    {
        int rc0 = tr_recv_exact(c, &h0, 1);
        if (c->tls) rdp_tls_set_timeout(c->tls, 8000);
        if (rc0 == -2) return 0;              /* nothing pending, not an error */
        if (rc0 != 0) return -1;
    }

    if (h0 == 0x03)
    {
        uint8_t hdr[3];
        size_t total, body;
        uint8_t* p;
        size_t n;

        if (tr_recv_exact(c, hdr, 3) != 0) return -1;
        total = ((size_t)hdr[1] << 8) | hdr[2];
        if (getenv("RDP_DUMP2"))
            LOG(c, "TPKT %02X %02X %02X %02X total=%u", h0, hdr[0], hdr[1], hdr[2],
                (unsigned)total);
        if (total < 4) return -1;
        body = total - 4;
        if (body > sizeof(c->rx)) return -1;
        if (body && tr_recv_exact(c, c->rx, body) != 0) return -1;

        p = c->rx;
        n = body;
        if (n >= 3 && p[0] == 0x02 && p[1] == 0xF0 && p[2] == 0x80)
        {
            p += 3;
            n -= 3;
        }
        if (n < 8 || p[0] != 0x68)
        {
            if (getenv("RDP_DUMP2"))
                LOG(c, "TPKT body %u bytes head %02X %02X %02X %02X %02X %02X %02X %02X",
                    (unsigned)n, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
            *data = p;
            *dlen = n;
            return 1;                         /* raw: not an MCS PDU */
        }
        {
            size_t ln, off;
            uint16_t ch = (uint16_t)(((unsigned)p[3] << 8) | p[4]);
            if (p[6] & 0x80)
            {
                ln = ((size_t)(p[6] & 0x7F) << 8) | p[7];
                off = 8;
            }
            else
            {
                ln = p[6];
                off = 7;
            }
            if (off + ln > n) ln = n - off;
            *chan = ch;
            *data = p + off;
            *dlen = ln;
        }
        return 1;
    }

    {
        uint8_t b1;
        size_t length, pos = 2, n;

        if (tr_recv_exact(c, &b1, 1) != 0) return -1;
        length = b1;
        if (getenv("RDP_DUMP2"))
            LOG(c, "FASTP h0=%02X b1=%02X", h0, b1);
        if (b1 & 0x80)
        {
            uint8_t b2;
            if (tr_recv_exact(c, &b2, 1) != 0) return -1;
            length = ((size_t)(b1 & 0x7F) << 8) | b2;
            pos = 3;
        }
        if (length < pos) return -1;
        n = length - pos;
        if (n > sizeof(c->rx)) return -1;
        if (n && tr_recv_exact(c, c->rx, n) != 0) return -1;
        *data = c->rx;
        *dlen = n;
        *is_fast = 1;
        (void)(h0 & 0xC0);                    /* encryption flags */
        return 1;
    }
}

int rdp_conn_pump(rdp_conn* c, int timeout_ms)
{
    uint16_t chan = 0;
    const uint8_t* data = NULL;
    size_t dlen = 0;
    int is_fast = 0;
    int rc;

    if (!c->connected) return -1;

    c->dirty = 0;
    c->surface.dirty_x0 = c->surface.dirty_x1 = 0;
    c->surface.dirty_y0 = c->surface.dirty_y1 = 0;
    c->wait_ms = timeout_ms > 0 ? timeout_ms : 1;
    rc = recv_pdu(c, &chan, &data, &dlen, &is_fast);
    if (rc > 0 && g_pdu_log < 400)
    {
        g_pdu_log++;
        if (is_fast)
            LOG(c, "pdu fast hdr=%02X len=%d", dlen > 0 ? data[0] : 0, (int)dlen);
        else if (dlen > 20)
            LOG(c, "pdu ch=%u pduType2=%u len=%d hdr=%02X%02X%02X%02X",
                chan, data[14], (int)dlen, data[0], data[1], data[2], data[3]);
    }
    if (rc == 0) return 0;                    /* no PDU arrived in time */
    if (rc < 0) return -1;
    if (getenv("RDP_DUMP"))
        LOG(c, "pdu fast=%d chan=%u len=%u %02X %02X %02X %02X %02X %02X",
            is_fast, chan, (unsigned)dlen,
            data ? data[0] : 0, data ? data[1] : 0, data ? data[2] : 0,
            data ? data[3] : 0, data ? data[4] : 0, data ? data[5] : 0);
    if (!data || dlen == 0) return 0;

    if (is_fast)
    {
        int n = rdp_update_fastpath(data, dlen, &c->surface,
                                    c->frag, RDP_FRAG_CAP,
                                    &c->frag_len, &c->frag_active);
        if (n > 0)
        {
            c->updates++;
            c->rects += (unsigned)n;
            c->dirty = 1;
            return 1;
        }
        return 0;
    }

    if (chan == 0)
        return 0;                             /* raw PDU, ignore */

    /* Server License Error PDU (preamble 0x80) - nothing to send back. */
    if (dlen >= 4 && data[0] == 0x80 && data[1] == 0 && data[2] == 0 && data[3] == 0)
    {
        LOG(c, "license pdu (%u bytes), ignored", (unsigned)dlen);
        return 0;
    }

    /* The 4-byte TS_SECURITY_HEADER is only present when the server expects
     * it (all-zero flags).  A share PDU starts 00 00 11 00 when it is absent. */
    {
        size_t off = 0;
        if (dlen >= 4 && data[0] == 0 && data[1] == 0 &&
            !(data[2] == 0x11 && data[3] == 0x00))
            off = 4;
        data += off;
        dlen -= off;
        c->use_sec_header = (off == 4);
    }

    if (dlen >= 4)
    {
        unsigned sc = (unsigned)(data[2] | (data[3] << 8));
        if (sc == 0x0011)                     /* Demand Active */
        {
            LOG(c, "demand active %u bytes", (unsigned)dlen);
            send_confirm_active(c, data, dlen);
            send_finalization(c);
            return 1;
        }
    }

    if (dlen < 19)
        return 0;

    {
        uint8_t pdu2 = data[14];
        const uint8_t* body = data + 18;
        size_t blen = dlen - 18;
        if (getenv("RDP_DUMP2"))
        {
            static int shown = 0;
            if (shown < 6)
            {
                shown++;
                LOG(c, "SRV data pdu: total=%u type2=0x%02X streamId=%u ucl=%u bodylen=%u",
                    (unsigned)(data[0] | (data[1] << 8)), (unsigned)pdu2,
                    (unsigned)data[11],
                    (unsigned)(data[12] | (data[13] << 8)), (unsigned)blen);
            }
        }
        if (pdu2 == 0x02)
            return handle_update(c, body, blen);
        if (pdu2 == 0x2F)
        {
            unsigned err = (unsigned)(body[0] | (body[1] << 8) |
                                      (body[2] << 16) | (body[3] << 24));
            LOG(c, "SET_ERROR_INFO 0x%08X", err);
            return 0;
        }
        if (pdu2 == 0x1F)
        {
            uint8_t sy[4];
            sy[0] = 0; sy[1] = 0; sy[2] = 0; sy[3] = 0;
            LOG(c, "SRV synchronize target=%u -> tx client synchronize",
                (unsigned)(body[0] | (body[1] << 8)));
            send_data_pdu(c, 0x1F, sy, 4);
            return 0;
        }
        if (pdu2 == 0x14)                     /* server control */
        {
            unsigned act = (unsigned)(body[0] | (body[1] << 8));
            const char* nm = act == 1 ? "REQUEST_CONTROL" :
                             act == 2 ? "GRANTED_CONTROL" :
                             act == 4 ? "COOPERATE" : "?";
            uint8_t ct[8];
            LOG(c, "SRV control %s (%u)", nm, act);
            if (act == 4)                     /* cooperate -> cooperate + request control */
            {
                ct[0] = 4; ct[1] = 0; ct[2] = 0; ct[3] = 0;
                ct[4] = 0; ct[5] = 0; ct[6] = 0; ct[7] = 0;
                send_data_pdu(c, 0x14, ct, 8);
                LOG(c, "tx control cooperate (re-ack)");
                ct[0] = 1; ct[1] = 0;             /* REQUEST_CONTROL */
                ct[2] = 0; ct[3] = 0;
                send_data_pdu(c, 0x14, ct, 8);
                LOG(c, "tx control request-control (re-ack)");
            }
            else if (act == 2)                /* granted control -> font list */
            {
                uint8_t fl[8];
                fl[0] = 0; fl[1] = 0; fl[2] = 0; fl[3] = 0;
                fl[4] = 3; fl[5] = 0; fl[6] = 0x32; fl[7] = 0;
                send_data_pdu(c, 0x27, fl, 8);
                LOG(c, "tx font list (after granted control)");
            }
            return 0;
        }
        if (pdu2 == 0x28)                      /* server font map */
        {
            uint8_t fl[8];
            fl[0] = 0; fl[1] = 0; fl[2] = 0; fl[3] = 0;
            fl[4] = 3; fl[5] = 0; fl[6] = 0x32; fl[7] = 0;
            send_data_pdu(c, 0x27, fl, 8);
            return 0;
        }
    }
    return 0;
}

unsigned rdp_conn_updates(rdp_conn* c) { return c ? c->updates : 0; }
unsigned rdp_conn_rects(rdp_conn* c) { return c ? c->rects : 0; }

/* One slow-path TS_INPUT_PDU carrying a single event.
 *   numberEvents(2) pad2Octets(2) event(12)
 * Sent as the data payload of a PDUTYPE2_INPUT (0x1C) share data PDU. */
static int send_input_event(rdp_conn* c, const uint8_t* ev, size_t elen)
{
    uint8_t body[64];
    size_t n = 0;

    if (elen == 0 || elen + 4 > sizeof(body)) return -1;
    body[n++] = 1; body[n++] = 0;         /* numberEvents */
    body[n++] = 0; body[n++] = 0;         /* pad2Octets   */
    memcpy(body + n, ev, elen);
    n += elen;
    if (getenv("RDP_INPUT_ON_USER"))
    {
        /* Some servers only take input on the user channel. */
        uint8_t out[1200];
        int w = rdp_mcs_build_send_data(out, sizeof(out), c->mcs.user_channel,
                                        c->mcs.user_channel, body, n);
        if (w <= 0) return -1;
        return tr_send(c, out, (size_t)w) < 0 ? -1 : 0;
    }
    return send_data_pdu(c, 0x1C, body, n);
}

int rdp_conn_refresh(rdp_conn* c, int left, int top, int right, int bottom)
{
    uint8_t body[32];
    size_t n = 0;

    body[n++] = 0; body[n++] = 1;                 /* numberRectangles = 1 */
    body[n++] = (uint8_t)left;  body[n++] = (uint8_t)(left >> 8);
    body[n++] = (uint8_t)top;   body[n++] = (uint8_t)(top >> 8);
    body[n++] = (uint8_t)right; body[n++] = (uint8_t)(right >> 8);
    body[n++] = (uint8_t)bottom;body[n++] = (uint8_t)(bottom >> 8);
    return send_data_pdu(c, 0x21, body, n);
}

int rdp_conn_key(rdp_conn* c, uint16_t scancode, int down, int extended)
{
    uint8_t fp[32];
    int n;

    if (getenv("RDP_SLOWPATH_INPUT"))
    {
        uint8_t ev[16];
        size_t en = rdp_input_event_keyboard(ev, sizeof(ev), scancode, down, extended);
        if (en == 0) return -1;
        return send_input_event(c, ev, en);
    }
    n = rdp_input_keyboard(fp, sizeof(fp), scancode, down, extended);
    if (n <= 0) return -1;
    return tr_send(c, fp, (size_t)n) < 0 ? -1 : 0;
}

/* Types one UTF-16 code unit into the remote session (FastPath SC_UNICODE).
 * Used by the on-screen keyboard path: the local layout does not matter,
 * Windows receives the characters directly. */
int rdp_conn_unicode(rdp_conn* c, uint16_t codeunit, int down)
{
    uint8_t fp[16];
    int n = rdp_input_unicode(fp, sizeof(fp), codeunit, down);
    if (n <= 0) return -1;
    return tr_send(c, fp, (size_t)n) < 0 ? -1 : 0;
}

int rdp_conn_mouse_move(rdp_conn* c, int x, int y){
    uint8_t fp[32];
    int n;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (c->cfg.width  > 0 && x >= c->cfg.width)  x = c->cfg.width  - 1;
    if (c->cfg.height > 0 && y >= c->cfg.height) y = c->cfg.height - 1;
    c->mouse_x = x;
    c->mouse_y = y;
    n = rdp_input_mouse_move(fp, sizeof(fp), (uint16_t)x, (uint16_t)y,
                             (uint16_t)c->cfg.width, (uint16_t)c->cfg.height);
    if (n <= 0) return -1;
    return tr_send(c, fp, (size_t)n) < 0 ? -1 : 0;
}

int rdp_conn_mouse_button(rdp_conn* c, int button, int down)
{
    uint8_t fp[32];
    int n;

    /* Windows applies the coordinates carried by the button event, so a click
     * sent with x=y=0 teleports the pointer to the top-left corner first.
     * Always repeat the last known position. */
    n = rdp_input_mouse_button_at(fp, sizeof(fp), button, down,
                                  (uint16_t)c->mouse_x, (uint16_t)c->mouse_y);
    if (n <= 0) return -1;
    return tr_send(c, fp, (size_t)n) < 0 ? -1 : 0;
}

int rdp_conn_mouse_wheel(rdp_conn* c, int notches)
{
    uint8_t fp[32];
    int n;

    /* Wheel events carry a position too: keep the pointer where it is. */
    n = rdp_input_mouse_wheel_at(fp, sizeof(fp), notches,
                                 (uint16_t)c->mouse_x, (uint16_t)c->mouse_y);
    if (n <= 0) return -1;
    return tr_send(c, fp, (size_t)n) < 0 ? -1 : 0;
}

void rdp_conn_free(rdp_conn* c)
{
    if (!c) return;
    if (c->tls) { rdp_tls_free(c->tls); c->tls = NULL; }
    if (c->sock >= 0) { close(c->sock); c->sock = -1; }
    free(c->framebuffer);
    free(c->frag);
    free(c);
}






