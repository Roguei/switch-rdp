/*
 * rdp_tls - minimal TLS wrapper over an existing TCP socket (mbedTLS).
 */

#ifndef RDP_TLS_H
#define RDP_TLS_H

#include <stddef.h>
#include <stdint.h>

typedef struct rdp_tls rdp_tls_t;

/* Wraps an already connected socket.  hostname is used for SNI only;
   the server certificate is not verified (RDP uses self-signed certs). */
rdp_tls_t* rdp_tls_new(int fd, const char* hostname);
int  rdp_tls_handshake(rdp_tls_t* t);
int  rdp_tls_write(rdp_tls_t* t, const void* buf, size_t len);
/* rdp_tls_read() returns:
 *   > 0  bytes read
 *     0  peer closed the connection
 *    -1  error
 *    -2  RDP_TLS_AGAIN: nothing came in within the current timeout
 */
#define RDP_TLS_AGAIN (-2)
int  rdp_tls_read(rdp_tls_t* t, void* buf, size_t len);

/* How long a single recv() may wait for data.  The desktop loop lowers this to
 * a few ms so that keyboard/stick sampling is not blocked behind the socket. */
void rdp_tls_set_timeout(rdp_tls_t* t, int ms);
/* Writes the server certificate's public key as a DER RSAPublicKey
 * (PKCS#1, i.e. i2d_PublicKey output) into out.  Used by CredSSP's
 * pubKeyAuth binding hash.  Returns 0 on success. */
int rdp_tls_server_pubkey(void* tls, uint8_t* out, size_t cap, size_t* outlen);

void rdp_tls_free(rdp_tls_t* t);

#endif
