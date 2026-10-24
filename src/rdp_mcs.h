/*
 * rdp_mcs - MCS (T.125) / GCC (T.124) layer for the switch_rdp client.
 *
 * This module knows how to:
 *   - build the "MCS Connect Initial" PDU carrying a GCC Conference Create
 *     Request with the Client Core / Security / Network / Cluster data,
 *   - parse the "MCS Connect Response" PDU, extracting the MCS channel ids,
 *     the server core data and the server's RSA public key from the
 *     proprietary certificate in the Server Security Data block,
 *   - build the Erect Domain / Attach User / Channel Join PDUs,
 *   - wrap/unwrap MCS SendData PDUs.
 *
 * Wire format references:
 *   [MS-RDPBCGR] 2.2.1.3   Client MCS Connect Initial PDU with GCC CCRq
 *   [MS-RDPBCGR] 2.2.1.4   Server MCS Connect Response PDU with GCC CCrsp
 *   [MS-RDPBCGR] 2.2.1.8   MCS Erect Domain Request
 *   [MS-RDPBCGR] 2.2.1.9   MCS Attach User Request/Confirm
 *   [MS-RDPBCGR] 2.2.1.10  MCS Channel Join Request/Confirm
 *   [MS-RDPBCGR] 2.2.1.11  MCS Send Data Request/Indication
 *   [T124] / [T125]        GCC conference data / MCS domain PDUs
 */

#ifndef RDP_MCS_H
#define RDP_MCS_H

#include <stdint.h>
#include <stddef.h>

#define RDP_MCS_MAX_CHANNELS 8

typedef struct {
    uint16_t user_channel;                 /* from Attach User Confirm */
    uint16_t io_channel;                   /* server channel id (ServerNetworkData MCSChannelId) */
    uint16_t channels[RDP_MCS_MAX_CHANNELS]; /* per requested channel id from ServerNetworkData */
    int      channel_count;

    /* server security data */
    int      encryption_method;            /* 0 none, 1 40bit, 2 128bit */
    int      encryption_level;             /* 1 low, 2 client, 3 high */
    int      rsa_bits;                     /* 0 if none / no certificate */
    uint8_t  rsa_modulus[256];             /* big-endian, rsa_bits/8 bytes */
    uint32_t rsa_exponent;                 /* typically 65537 */

    int      desktop_width, desktop_height;
    int      server_build;
} rdp_mcs_info;

/* Build a complete "MCS Connect Initial" PDU (TPKT + X.224 DT + BER).
   out/outcap: destination buffer. Returns bytes written, or -1.
   channel_names: array of 7-char ASCII names, e.g. "rdpdr", "cliprdr".
   The names are stored as channel_count consecutive 8-byte records
   (7 name bytes + NUL, as in a CHANNEL_DEF name field).
   Writes the GCC conference name "1" as required. */
void rdp_mcs_set_selected_protocol(uint32_t p);

int rdp_mcs_build_connect_initial(uint8_t* out, size_t outcap,
                                  int width, int height, int color_depth,
                                  const char* channel_names, int channel_count,
                                  const char* client_name);

/* Parse a received "MCS Connect Response" payload (the X.224 DT payload,
   i.e. starting at the BER data, not including TPKT header).
   Fills info. Returns 0 on success. */
int rdp_mcs_parse_connect_response(const uint8_t* data, size_t len,
                                   rdp_mcs_info* info);

/* Small PDU builders; return bytes written or -1. outcap >= 64. */
int rdp_mcs_build_erect_domain(uint8_t* out, size_t outcap,
                               uint16_t user_channel);
int rdp_mcs_build_attach_user(uint8_t* out, size_t outcap);
int rdp_mcs_parse_attach_user_confirm(const uint8_t* data, size_t len,
                                      uint16_t* user_channel);
int rdp_mcs_build_channel_join(uint8_t* out, size_t outcap,
                               uint16_t user_channel, uint16_t channel_id);

/* Wrap an arbitrary payload into an MCS SendData PDU (0x64) on the given
   channel. Returns bytes written or -1. */
int rdp_mcs_build_send_data(uint8_t* out, size_t outcap,
                            uint16_t user_channel, uint16_t channel_id,
                            const uint8_t* payload, size_t payload_len);

/* If the received TPKT payload contains an MCS SendData PDU, return a
   pointer to its user data and its length via out/outlen. Returns 0. */
int rdp_mcs_extract_send_data(const uint8_t* tpkt_payload, size_t len,
                              const uint8_t** out, size_t* outlen);
#endif
