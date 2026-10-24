/*
 * rdp_planar - RDP6 "Planar" bitmap codec (decoder only).
 *
 * Ported from FreeRDP 3.5.1 (libfreerdp/codec/planar.c and
 * libfreerdp/primitives/prim_YCoCg.c).  No external dependencies.
 *
 * A compressed TS_BITMAP_DATA payload starts with a format header byte:
 *
 *     bits 0-2  CLL  colour loss level (0 => planes are plain R,G,B)
 *     bit  3    CS   chroma subsampling (YCoCg, Co/Cg at half resolution)
 *     bit  4    RLE  planes are run-length encoded and delta coded per row
 *     bit  5    NA   no alpha (clear => a fourth plane is present)
 *
 * Plane order in the stream: [alpha] Y(luma) Co Cg.  Each plane is encoded
 * independently; when RLE is used a plane is self-delimiting (its decoder
 * consumes exactly the bytes it needs).
 */

#ifndef RDP_PLANAR_H
#define RDP_PLANAR_H

#include <stdint.h>
#include <stddef.h>

/* Decodes one planar tile into `rgb` as w*h*3 bytes, R,G,B, top-down.
 * The source rows are stored bottom-up, so the result is flipped vertically
 * (dest row 0 is the top of the tile).
 * Returns 0 on success, negative on error. */
int rdp_planar_decode(const uint8_t* data, size_t len, int w, int h, uint8_t* rgb);

#endif
