/*
 * rdp_update - Fast-Path update parsing and bitmap decoding.
 *
 * Wire format confirmed against a live Windows server and FreeRDP 3.5.1:
 *
 *   Fast-Path frame   : 1 flags byte (bits 7-6 encryption, bits 5-2
 *                       numberEvents), then a PER-encoded length (one byte,
 *                       or two when the high bit of the first is set).  The
 *                       length counts every byte after the flags byte.
 *   update list       : a bare sequence of
 *                         updateHeader(1) [compressionFlags(1)] size(2) data
 *                       with no numberOfUpdates byte.
 *   updateHeader      : code (bits 3-0), fragmentation (bits 5-4),
 *                       compression (bits 7-6).
 *   fragmentation     : SINGLE=0 LAST=1 FIRST=2 NEXT=3.
 *
 * Bitmap updates (code 1) are a TS_UPDATE_BITMAP: updateType(2),
 * numberRectangles(2), then TS_BITMAP_DATA rectangles.  Compressed
 * rectangles use the RDP6 Planar codec (rdp_planar.c).
 */

#ifndef RDP_UPDATE_H
#define RDP_UPDATE_H

#include <stdint.h>
#include <stddef.h>
#include "rdp_bitmap.h"

/* Parses a Fast-Path update payload and applies bitmap updates to `surface`.
 *
 * A logical update is split by the server into FIRST/NEXT/LAST fragments of
 * up to ~16 KiB each; a full-screen 1024x768 repaint arrives as three of them
 * (~47 KiB in total), so the reassembly buffer must be much bigger than one
 * fragment.  `frag_buf` is a caller-owned buffer of `frag_cap` bytes.
 *
 * Returns the number of bitmap rectangles applied. */
int rdp_update_fastpath(const uint8_t* payload, size_t len,
                        rdp_surface* surface,
                        uint8_t* frag_buf, size_t frag_cap,
                        size_t* frag_len, int* frag_active);

/* Applies one TS_UPDATE_BITMAP body (starting at updateType) to `surface`.
 * Returns the number of rectangles applied, or -1. */
int rdp_update_bitmap(const uint8_t* body, size_t len, rdp_surface* surface);

#endif
