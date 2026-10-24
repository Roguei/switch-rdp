#ifndef RDP_BITMAP_H
#define RDP_BITMAP_H
#include <stdint.h>
#include <stddef.h>

/* Destination: width*height RGBA8888 pixels, row stride in bytes = width*4.
 *
 * The dirty box (inclusive-exclusive, clipped) accumulates every rectangle
 * written by a bitmap update, so the Switch side can copy just the changed
 * area to the framebuffer instead of the whole 1280x720x4 buffer. */
typedef struct {
    uint8_t* pixels;
    int      width;
    int      height;
    int      dirty_x0, dirty_y0, dirty_x1, dirty_y1;
} rdp_surface;

/* Decodes one bitmapData block.
   data/len: the whole "bitmapData" payload (starting at the
   updateType u16 = 0x0001 BITMAP_UPDATE).
   clip: rectangle of the surface that may be written.
   Returns 0 on success, -1 on parse error. */
int rdp_bitmap_process(const uint8_t* data, size_t len, rdp_surface* surface);
#endif
