/*
 * rdp_nla - CredSSP / NTLMv2 authentication for RDP with NLA enabled.
 *
 * Flow (inside the TLS tunnel):
 *   client -> TSRequest{ negoTokens(NTLMSSP NEGOTIATE) }
 *   server -> TSRequest{ negoTokens(NTLMSSP CHALLENGE) }
 *   client -> TSRequest{ negoTokens(NTLMSSP AUTHENTICATE), pubKeyAuth }
 *   server -> TSRequest{ pubKeyAuth, errorCode }
 *
 * Only the pieces RDP needs are implemented.
 */

#ifndef RDP_NLA_H
#define RDP_NLA_H

#include <stdint.h>
#include <stddef.h>

typedef struct
{
    uint8_t  nt_hash[16];       /* MD4(UTF16LE(password)) */
    uint8_t  session_key[16];   /* NTLM session base key */
    uint8_t  exported_key[16];
    const char* user;
    const char* pass;
    const char* domain;
} nla_cred;

/* Optional logging hook (set from the session layer). */
extern void (*nla_log)(const char* msg);

/* Performs the whole CredSSP exchange over an established TLS session.
   tls_ctx is an rdp_tls_t*. Returns 0 on success. */
int nla_authenticate(void* tls_ctx, const nla_cred* cred,
                     const uint8_t* server_pubkey_hash /* optional, may be NULL */);

#endif

