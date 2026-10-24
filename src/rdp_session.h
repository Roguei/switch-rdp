/*
 * rdp_session - RDP connection orchestration for switch_rdp.
 *
 * Layers:
 *   TCP  ->  X.224 negotiation (NLA/HYBRID)  ->  TLS (mbedTLS)
 *        ->  CredSSP/NTLMv2 (rdp_nla)  ->  MCS (rdp_mcs)
 *        ->  Client Info  ->  capability exchange  ->  update loop
 */

#ifndef RDP_SESSION_H
#define RDP_SESSION_H

#include <stdint.h>
#include <stddef.h>
#include "rdp_bitmap.h"

/* failure reasons handed to the UI */
enum
{
    RDP_ERR_ADDRESS = 1,   /* bad IP / host name */
    RDP_ERR_NETWORK,       /* connect timeout */
    RDP_ERR_REFUSED,       /* port closed */
    RDP_ERR_UNREACH,       /* no route */
    RDP_ERR_X224,          /* not an RDP port */
    RDP_ERR_TLS,
    RDP_ERR_AUTH,          /* NLA rejected: wrong password / no RDP right */
    RDP_ERR_ACL,
    RDP_ERR_MCS,
    RDP_ERR_CANCELLED,
    RDP_ERR_OTHER
};

typedef struct
{
    const char* host;
    int         port;
    const char* user;
    const char* pass;
    const char* domain;      /* may be NULL/empty */
    int         width;
    int         height;
} rdp_cfg;

typedef void (*rdp_stage_fn)(const char* text, void* user);

/* 1 while the libnx console is usable.  Set it to 0 before consoleExit(). */
extern int rdp_console_live;

typedef struct rdp_conn rdp_conn;

rdp_conn* rdp_conn_new(const rdp_cfg* cfg, rdp_stage_fn stage, void* user);

/* Progress text for the UI (may be NULL).  Called while connecting. */
extern void (*rdp_ui_stage_hook)(const char* text);
/* Polled while connecting; non-zero aborts the attempt (RDP_ERR_CANCELLED). */
extern int  (*rdp_cancel_hook)(void);

/* 0 = drop the bitmap-codecs capset, 1 = send it empty, 2 = echo it. */
extern int rdp_codec_mode;

/* Runs the whole connection sequence.  Returns 0 on success, else one of the
 * RDP_ERR_* codes above. */
int  rdp_conn_connect(rdp_conn* c);

/* Returns the surface the client renders into (RGBA8888, width*4 stride). */
rdp_surface* rdp_conn_surface(rdp_conn* c);

/* Processes pending data for up to timeout_ms. Returns 1 if the frame
   changed, 0 if nothing happened, -1 if the connection ended. */
int  rdp_conn_pump(rdp_conn* c, int timeout_ms);

/* Statistics for diagnostics. */
unsigned rdp_conn_updates(rdp_conn* c);
unsigned rdp_conn_rects(rdp_conn* c);

/* Asks the server to repaint a rectangle (Client Refresh Rect PDU). */
int  rdp_conn_refresh(rdp_conn* c, int left, int top, int right, int bottom);

/* Sends a keyboard event (PC set-1 scancode). */
int  rdp_conn_key(rdp_conn* c, uint16_t scancode, int down, int extended);

/* Sends an absolute mouse move. */
int  rdp_conn_mouse_move(rdp_conn* c, int x, int y);

/* Sends a mouse button event: 1 left, 2 right, 3 middle. */
int  rdp_conn_mouse_button(rdp_conn* c, int button, int down);

/* Sends a wheel rotation at the current pointer position.  notches > 0 scrolls
 * up (away from the user), < 0 scrolls down. */
int  rdp_conn_mouse_wheel(rdp_conn* c, int notches);

/* Types one UTF-16 code unit (FastPath SC_UNICODE).  Send a 1 then a 0 per
 * character.  This is how the on-screen keyboard text reaches the server. */
int  rdp_conn_unicode(rdp_conn* c, uint16_t codeunit, int down);

void rdp_conn_free(rdp_conn* c);

#endif
