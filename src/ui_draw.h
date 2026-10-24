#ifndef UI_DRAW_H
#define UI_DRAW_H

#include <stdint.h>

/* The whole UI is drawn by hand into an RGBA8888 target: the framebuffer's
 * linear buffer (see main.c).  Coordinates are pixels in that target. */

void ui_target(void* dst, uint32_t stride, int w, int h);
void ui_clear(uint8_t r, uint8_t g, uint8_t b);
void ui_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void ui_frame(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* UTF-8 in, glyphs from ui_font.c.  scale 1 = 16 px tall, 2 = 32 px (the UI
 * uses 2 for everything so it stays readable on the small screen). */
void ui_text(int x, int y, int scale, uint8_t r, uint8_t g, uint8_t b, const char* s);
void ui_text_center(int cx, int y, int scale, uint8_t r, uint8_t g, uint8_t b, const char* s);
int  ui_text_w(const char* s, int scale);

#endif
