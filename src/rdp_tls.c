/*
 * rdp_tls - minimal TLS wrapper over an existing TCP socket (mbedTLS 2.28).
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include <sys/socket.h>
#include <poll.h>
#include <errno.h>
#include <unistd.h>

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>

#include "rdp_tls.h"
#include "rdp_session.h"   /* rdp_console_live */

struct rdp_tls
{
    mbedtls_ssl_context       ssl;
    mbedtls_ssl_config        conf;
    mbedtls_entropy_context   entropy;
    mbedtls_ctr_drbg_context  ctr_drbg;
    int fd;
    int inited_conf;
    int timeout_ms;          /* per-recv() poll budget */
};

static int tls_wait(int fd, short ev, int ms)
{
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = ev;
    pfd.revents = 0;
    return poll(&pfd, 1, ms) > 0 ? 0 : -1;
}

/* Writes one TLS record fragment.  Every failure path has to return *some*
 * error: the caller (rdp_tls_write) retries on WANT_WRITE, so a callback that
 * keeps reporting WANT_WRITE while the peer has stopped reading turns into an
 * infinite loop that freezes the whole client - which is exactly what happened
 * when the server's receive window stayed closed while it was busy painting.
 * A non-recoverable errno (EPIPE, ECONNRESET, ...) is reported as -1 right
 * away, and a writable-looking socket that never drains is given up on. */
static int tls_send_cb(void* ctx, const unsigned char* buf, size_t len)
{
    rdp_tls_t* t = (rdp_tls_t*)ctx;
    int fd = t->fd;
    size_t done = 0;
    int waits = 0;
    while (done < len)
    {
        int n = (int)send(fd, buf + done, len - done, 0);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOBUFS)
                return -1;
            if (++waits > 4) return -1;
            if (tls_wait(fd, POLLOUT, 2000) != 0) return -1;
            continue;
        }
        if (n == 0) return -1;
        done += (size_t)n;
    }
    return (int)len;
}

static int tls_recv_cb(void* ctx, unsigned char* buf, size_t len)
{
    rdp_tls_t* t = (rdp_tls_t*)ctx;
    int fd = t->fd;
    for (;;)
    {
        int n = (int)recv(fd, buf, len, 0);
        if (n > 0) return n;
        if (n == 0) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
        if (tls_wait(fd, POLLIN, t->timeout_ms > 0 ? t->timeout_ms : 1) != 0)
            return MBEDTLS_ERR_SSL_WANT_READ;
    }
}

/* Deterministic-but-adequate PRNG for the TLS client random.
   mbedtls entropy on this platform goes through svcGetInfo(RandomEntropy),
   which the emulator does not handle well, so we keep it in userspace. */
static uint64_t g_rng_state = 0x9E3779B97F4A7C15ULL;

static int rdp_rng(void* ctx, unsigned char* out, size_t len)
{
    (void)ctx;
    static int seeded = 0;
    if (!seeded)
    {
        seeded = 1;
#ifdef __SWITCH__
        uint64_t t = (uint64_t)svcGetSystemTick();
#else
        uint64_t t = (uint64_t)clock();
#endif
        g_rng_state ^= t * 0x2545F4914F6CDD1DULL;
    }
    for (size_t i = 0; i < len; i++)
    {
        g_rng_state ^= g_rng_state << 13;
        g_rng_state ^= g_rng_state >> 7;
        g_rng_state ^= g_rng_state << 17;
        out[i] = (unsigned char)(g_rng_state >> 24);
    }
    return 0;
}
rdp_tls_t* rdp_tls_new(int fd, const char* hostname)
{
    rdp_tls_t* t = (rdp_tls_t*)calloc(1, sizeof(rdp_tls_t));
    if (!t) return NULL;

    t->fd = fd;
    if (rdp_console_live) fprintf(stderr, "rdp_tls: init");
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_entropy_init(&t->entropy);
    mbedtls_ctr_drbg_init(&t->ctr_drbg);

    /* userspace RNG only: no mbedtls entropy / ctr_drbg */


    if (rdp_console_live) fprintf(stderr, "rdp_tls: defaults");
    if (mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0)
    {
        rdp_tls_free(t);
        return NULL;
    }
    t->inited_conf = 1;

    /* RDP servers use self-signed certificates: no verification. */
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&t->conf, rdp_rng, NULL);
    mbedtls_ssl_conf_min_version(&t->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_1);   /* TLS 1.0+ */

    if (rdp_console_live) fprintf(stderr, "rdp_tls: setup");
    if (mbedtls_ssl_setup(&t->ssl, &t->conf) != 0)
    {
        rdp_tls_free(t);
        return NULL;
    }
    if (hostname && hostname[0])
        mbedtls_ssl_set_hostname(&t->ssl, hostname);

    t->timeout_ms = 8000;
    mbedtls_ssl_set_bio(&t->ssl, t, tls_send_cb, tls_recv_cb, NULL);
    return t;
}

int rdp_tls_handshake(rdp_tls_t* t)
{
    int rc;
    while ((rc = mbedtls_ssl_handshake(&t->ssl)) != 0)
    {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE)
            return rc;
    }
    return 0;
}

int rdp_tls_write(rdp_tls_t* t, const void* buf, size_t len)
{
    size_t done = 0;
    int retries = 0;

    while (done < len)
    {
        int n = mbedtls_ssl_write(&t->ssl, (const unsigned char*)buf + done,
                                  len - done);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            /* bounded: the send callback already waited for POLLOUT, so a
             * repeated WANT_WRITE means the peer is gone or stuck */
            if (++retries > 8) return -1;
            continue;
        }
        retries = 0;
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return (int)done;
}

int rdp_tls_read(rdp_tls_t* t, void* buf, size_t len)
{
    int n = mbedtls_ssl_read(&t->ssl, (unsigned char*)buf, len);
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
        return RDP_TLS_AGAIN;
    return n;
}

void rdp_tls_set_timeout(rdp_tls_t* t, int ms)
{
    if (t) t->timeout_ms = ms > 0 ? ms : 1;
}

/* DER helpers for the PKCS#1 RSAPublicKey blob used by CredSSP. */

/* ---- DER helpers for the PKCS#1 RSAPublicKey blob used by CredSSP ---- */
static size_t der_len(uint8_t* p, size_t len)
{
    if (len < 0x80) { p[0] = (uint8_t)len; return 1; }
    if (len <= 0xFF) { p[0] = 0x81; p[1] = (uint8_t)len; return 2; }
    if (len <= 0xFFFF) { p[0] = 0x82; p[1] = (uint8_t)(len >> 8); p[2] = (uint8_t)len; return 3; }
    p[0] = 0x83; p[1] = (uint8_t)(len >> 16); p[2] = (uint8_t)(len >> 8); p[3] = (uint8_t)len;
    return 4;
}

/* Emits DER INTEGER for the big-endian magnitude in `be`. */
static size_t der_int(uint8_t* out, size_t cap, const uint8_t* be, size_t belen)
{
    uint8_t tmp[1200];
    size_t n = 0, lb;
    int pad;

    while (belen > 1 && be[0] == 0) { be++; belen--; }
    pad = (be[0] & 0x80) ? 1 : 0;
    if (1 + 4 + pad + belen > cap) return 0;
    tmp[n++] = 0x02;
    lb = der_len(tmp + n, belen + (size_t)pad);
    n += lb;
    if (pad) tmp[n++] = 0;
    memcpy(tmp + n, be, belen);
    n += belen;
    memcpy(out, tmp, n);
    return n;
}

int rdp_tls_server_pubkey(void* tls, uint8_t* out, size_t cap, size_t* outlen)
{
    rdp_tls_t* t = (rdp_tls_t*)tls;
    const mbedtls_x509_crt* crt;
    mbedtls_rsa_context* rsa;
    uint8_t nbuf[1024], ebuf[16], body[1400];
    size_t nlen, elen, bl = 0, lb;

    if (!t) return -1;
    crt = mbedtls_ssl_get_peer_cert(&t->ssl);
    if (!crt) return -1;
    rsa = mbedtls_pk_rsa(crt->pk);
    if (!rsa) return -1;

    nlen = mbedtls_mpi_size(&rsa->N);
    elen = mbedtls_mpi_size(&rsa->E);
    if (nlen > sizeof(nbuf) || elen > sizeof(ebuf)) return -1;
    if (mbedtls_mpi_write_binary(&rsa->N, nbuf, nlen) != 0) return -1;
    if (mbedtls_mpi_write_binary(&rsa->E, ebuf, elen) != 0) return -1;

    bl += der_int(body + bl, sizeof(body) - bl, nbuf, nlen);
    if (!bl) return -1;
    {
        size_t e = der_int(body + bl, sizeof(body) - bl, ebuf, elen);
        if (!e) return -1;
        bl += e;
    }

    /* SEQUENCE { ... } */
    if (2 + 4 + bl > cap) return -1;
    out[0] = 0x30;
    lb = der_len(out + 1, bl);
    memcpy(out + 1 + lb, body, bl);
    *outlen = 1 + lb + bl;
    return 0;
}

void rdp_tls_free(rdp_tls_t* t)
{
    if (!t) return;
    mbedtls_ssl_free(&t->ssl);
    if (t->inited_conf) mbedtls_ssl_config_free(&t->conf);
    mbedtls_ctr_drbg_free(&t->ctr_drbg);
    mbedtls_entropy_free(&t->entropy);
    free(t);
}
