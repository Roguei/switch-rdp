/*
 * rdp_update.c - Fast-Path update parsing and bitmap decoding.
 * See rdp_update.h for the wire format notes.
 */

#include "rdp_update.h"
#include "rdp_planar.h"

#include <string.h>
#include <stdio.h>

int rdp_update_other_codes[16];
void (*rdp_update_log_hook)(const char* line);
int rdp_update_debug_log = 1;   /* rect-log: log the first rectangles */
static void ulog7(const char* fmt, int a, int b, int c, int d, int e, int f, int g)
{
    char line[160];
    if (!rdp_update_log_hook) return;
    snprintf(line, sizeof(line), fmt, a, b, c, d, e, f, g);
    rdp_update_log_hook(line);
}
/* The SD trace file lives in main.c; stderr does not go there, so hand the
 * rect lines to a hook instead of fprintf (that is why they never showed up). */
void (*rdp_update_log_hook)(const char* line) = 0;   /* set by main.c */
#include <stdlib.h>

#define UPD_CODE_BITMAP 1
#define UPD_FRAG_SINGLE 0
#define UPD_FRAG_LAST   1
#define UPD_FRAG_FIRST  2
#define UPD_FRAG_NEXT   3

#define TILE_MAX_W 128
#define TILE_MAX_H 128

static uint8_t s_tile[TILE_MAX_W * TILE_MAX_H * 3];

/* grow the destination dirty box (already clipped by the caller) */
static void mark_dirty(rdp_surface* s, int x0, int y0, int x1, int y1)
{
    if (x1 <= x0 || y1 <= y0) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->width)  x1 = s->width;
    if (y1 > s->height) y1 = s->height;
    if (x1 <= x0 || y1 <= y0) return;
    if (s->dirty_x1 <= s->dirty_x0)
    {
        s->dirty_x0 = x0; s->dirty_y0 = y0;
        s->dirty_x1 = x1; s->dirty_y1 = y1;
        return;
    }
    if (x0 < s->dirty_x0) s->dirty_x0 = x0;
    if (y0 < s->dirty_y0) s->dirty_y0 = y0;
    if (x1 > s->dirty_x1) s->dirty_x1 = x1;
    if (y1 > s->dirty_y1) s->dirty_y1 = y1;
}

static void blit(rdp_surface* s, int dl, int dt, int w, int h, const uint8_t* rgb)
{
    int y, x;

    mark_dirty(s, dl, dt, dl + w, dt + h);

    /* Fast path: the whole tile is inside the surface (always true for the
     * tiles the server sends).  The clipping below costs three compares per
     * pixel, which is a measurable share of a full 1280x720 repaint. */
    if (dl >= 0 && dt >= 0 && dl + w <= s->width && dt + h <= s->height)
    {
        for (y = 0; y < h; y++)
        {
            uint8_t* d = s->pixels + ((size_t)(dt + y) * (size_t)s->width
                                      + (size_t)dl) * 4;
            const uint8_t* src = rgb + (size_t)y * (size_t)w * 3;
            for (x = 0; x < w; x++)
            {
                d[0] = src[0];
                d[1] = src[1];
                d[2] = src[2];
                d[3] = 0xFF;
                d += 4;
                src += 3;
            }
        }
        return;
    }

    for (y = 0; y < h; y++)
    {
        /* rdp_planar_decode() already hands back top-down rows for compressed
         * rectangles (verified pixel-for-pixel against the reference frame), so
         * copy straight out.  Only the raw path below needs a row flip. */
        int dy = dt + y;
        if (dy < 0 || dy >= s->height) continue;
        for (x = 0; x < w; x++)
        {
            int dx = dl + x;
            uint8_t* d;
            const uint8_t* src;
            if (dx < 0 || dx >= s->width) continue;
            d = s->pixels + ((size_t)dy * (size_t)s->width + (size_t)dx) * 4;
            src = rgb + ((size_t)y * (size_t)w + (size_t)x) * 3;
            d[0] = src[0];
            d[1] = src[1];
            d[2] = src[2];
            d[3] = 0xFF;
        }
    }
}

static void blit_raw(rdp_surface* s, int dl, int dt, int w, int h, int bpp,
                     const uint8_t* src, size_t srclen)
{
    /* RDP bitmap rows are padded to a 4-byte boundary (TS_BITMAP_DATA); without
     * this every row shifts a little and the picture turns into diagonal
     * garbage - the "one weird coloured block" seen on RDP 5 servers. */
    int rowbytes = ((w * bpp + 7) / 8 + 3) & ~3;
    int y, x;

    mark_dirty(s, dl, dt, dl + w, dt + h);
    for (y = 0; y < h; y++)
    {
        int dy = dt + (h - 1 - y);          /* bitmap rows are bottom-up */
        const uint8_t* row;
        if ((size_t)(y + 1) * (size_t)rowbytes > srclen) break;
        if (dy < 0 || dy >= s->height) continue;
        row = src + (size_t)y * (size_t)rowbytes;
        for (x = 0; x < w; x++)
        {
            int dx = dl + x;
            uint8_t* d;
            uint8_t r, g, b;
            if (dx < 0 || dx >= s->width) continue;
            if (bpp == 32 || bpp == 24)
            {
                const uint8_t* p = row + (size_t)x * (size_t)(bpp / 8);
                b = p[0]; g = p[1]; r = p[2];
            }
            else
            {
                const uint8_t* p = row + (size_t)x * (size_t)(bpp / 8);
                unsigned v = (unsigned)p[0] | ((unsigned)p[1] << 8);
                if (bpp == 16)              /* RGB565 */
                {
                    r = (uint8_t)(((v >> 11) & 0x1F) << 3);
                    g = (uint8_t)(((v >> 5) & 0x3F) << 2);
                    b = (uint8_t)((v & 0x1F) << 3);
                }
                else                        /* bpp == 15: RGB555 */
                {
                    r = (uint8_t)(((v >> 10) & 0x1F) << 3);
                    g = (uint8_t)(((v >> 5) & 0x1F) << 3);
                    b = (uint8_t)((v & 0x1F) << 3);
                }
            }
            d = s->pixels + ((size_t)dy * (size_t)s->width + (size_t)dx) * 4;
            d[0] = r; d[1] = g; d[2] = b; d[3] = 0xFF;
        }
    }
}


/* ------------------------------------------------------------------ */
/* Interleaved RLE - the RLE_BITMAP_STREAM of MS-RDPEGDI that Windows  */
/* RDP 5 servers send for BITMAP_COMPRESSION | NO_BITMAP_COMPRESSION_HDR. */
/* (flags & 0x401).                                                    */
/*                                                                     */
/* This is not a colour/count list: every order is relative to the row */
/* above.  Background runs copy the pixel from the previous row (black */
/* on the first line), foreground runs XOR the pixel from the previous */
/* row with the running foreground colour, image orders paint a bit    */
/* mask, and the client has to keep an "insert foreground pel" flag so */
/* that two background runs in a row produce a dithered seam.  The port */
/* below follows FreeRDP's RLEDECOMPRESS() order for order, which is   */
/* what the reference clients use, so the tiles come out identical.    */
/* ------------------------------------------------------------------ */
#define RLE_BG_RUN                 0x00
#define RLE_FG_RUN                 0x01
#define RLE_FGBG_IMAGE             0x02
#define RLE_COLOR_RUN              0x03
#define RLE_COLOR_IMAGE            0x04
#define RLE_LITE_SET_FG_FG_RUN     0x0C
#define RLE_LITE_SET_FG_FGBG_IMAGE 0x0D
#define RLE_LITE_DITHERED_RUN      0x0E
#define RLE_MEGA_BG_RUN            0xF0
#define RLE_MEGA_FG_RUN            0xF1
#define RLE_MEGA_FGBG_IMAGE        0xF2
#define RLE_MEGA_COLOR_RUN         0xF3
#define RLE_MEGA_COLOR_IMAGE       0xF4
#define RLE_MEGA_SET_FG_RUN        0xF6
#define RLE_MEGA_SET_FGBG_IMAGE    0xF7
#define RLE_MEGA_DITHERED_RUN      0xF8
#define RLE_SPECIAL_FGBG_1         0xF9
#define RLE_SPECIAL_FGBG_2         0xFA
#define RLE_SPECIAL_WHITE          0xFD
#define RLE_SPECIAL_BLACK          0xFE

typedef struct
{
    const uint8_t* s;
    size_t n;
    size_t p;
    int pb;                 /* pixel size in bytes */
    int bad;
} rle_src;

/* Read one pixel.  The value keeps the little-endian byte order of the
 * stream, so the conversion below has to undo it per depth. */
static uint32_t rle_px(rle_src* c)
{
    uint32_t v = 0;
    int i;
    if (c->p + (size_t)c->pb > c->n) { c->bad = 1; return 0; }
    for (i = 0; i < c->pb; i++)
        v |= (uint32_t)c->s[c->p + (size_t)i] << (8 * i);
    c->p += (size_t)c->pb;
    return v;
}

static int rle_code(unsigned hdr)
{
    if ((hdr & 0xC0u) != 0xC0u) return (int)(hdr >> 5);
    if ((hdr & 0xF0u) == 0xF0u) return (int)hdr;
    return (int)(hdr >> 4);
}

/* Runs lengths are masked out of the order byte; a zero field means an
 * extra byte (regular / lite) or a 16-bit word (mega) follows. */
static int rle_len(int code, rle_src* c, uint32_t* out)
{
    uint32_t rl;
    uint8_t hdr = c->s[c->p];
    size_t p = c->p + 1;

    if (code == RLE_FGBG_IMAGE || code == RLE_LITE_SET_FG_FGBG_IMAGE)
    {
        rl = hdr & ((code == RLE_FGBG_IMAGE) ? 0x1Fu : 0x0Fu);
        if (rl == 0)
        {
            if (p + 1 > c->n) return -1;
            rl = (uint32_t)c->s[p] + 1;
            p++;
        }
        else rl *= 8;
    }
    else if (code == RLE_BG_RUN || code == RLE_FG_RUN ||
             code == RLE_COLOR_RUN || code == RLE_COLOR_IMAGE)
    {
        rl = hdr & 0x1Fu;
        if (rl == 0)
        {
            if (p + 1 > c->n) return -1;
            rl = (uint32_t)c->s[p] + 32;
            p++;
        }
    }
    else if (code == RLE_LITE_SET_FG_FG_RUN || code == RLE_LITE_DITHERED_RUN)
    {
        rl = hdr & 0x0Fu;
        if (rl == 0)
        {
            if (p + 1 > c->n) return -1;
            rl = (uint32_t)c->s[p] + 16;
            p++;
        }
    }
    else                        /* 0xF0..0xFE: 16-bit run length */
    {
        if (p + 2 > c->n) return -1;
        rl = (uint32_t)c->s[p] | ((uint32_t)c->s[p + 1] << 8);
        p += 2;
    }
    c->p = p;
    *out = rl;
    return 0;
}

/* Decoded tile in stream order (row 0 = bottom row of the bitmap). */
static uint32_t s_rle_px[TILE_MAX_W * TILE_MAX_H];

static int rle_decompress(const uint8_t* src, size_t len, int w, int h, int bpp,
                          uint8_t* rgb)
{
    rle_src c;
    uint32_t* px = s_rle_px;
    uint32_t total, row, di = 0, rl = 0, fg = 0;
    uint32_t black, white;
    int first = 1, ins = 0, code, x, y;

    if (w <= 0 || h <= 0 || w > TILE_MAX_W || h > TILE_MAX_H) return -1;
    if (!src || !len) return -1;

    total = (uint32_t)w * (uint32_t)h;
    row = (uint32_t)w;

    c.s = src; c.n = len; c.p = 0; c.bad = 0;
    c.pb = (bpp == 15 || bpp == 16) ? 2 : bpp / 8;
    if (c.pb < 1 || c.pb > 4) return -1;

    switch (c.pb)
    {
    case 1:  black = 0x00u;       white = 0xFFu;       break;
    case 2:  black = 0x0000u;     white = 0xFFFFu;     break;
    case 3:  black = 0x000000u;   white = 0xFFFFFFu;   break;
    default: black = 0x00000000u; white = 0xFFFFFFFFu; break;
    }
    /* The running foreground colour starts out white (same as the reference
     * clients); an order that uses it before any SET_FG order would otherwise
     * paint black over a white area. */
    fg = white;
    memset(px, 0, (size_t)total * sizeof(uint32_t));

    while (c.p < c.n)
    {
        /* The first line has nothing above it, so it is painted in absolute
         * colours; every later line works by copying/XORing the line above. */
        if (first && di >= row) { first = 0; ins = 0; }

        code = rle_code(c.s[c.p]);

        if (code == RLE_BG_RUN || code == RLE_MEGA_BG_RUN)
        {
            if (rle_len(code, &c, &rl)) return -1;
            if (first)
            {
                if (ins)                        /* finish the dithered seam */
                {
                    if (di >= total) return -1;
                    px[di++] = black;
                    rl--;
                }
                if (di + rl > total) return -1;
                while (rl--) px[di++] = black;
            }
            else
            {
                if (ins)
                {
                    if (di >= total || di < row) return -1;
                    px[di] = px[di - row] ^ fg;
                    di++;
                    rl--;
                }
                if (di + rl > total) return -1;
                while (rl--) { px[di] = px[di - row]; di++; }
            }
            ins = 1;
            continue;
        }

        ins = 0;

        switch (code)
        {
        case RLE_FG_RUN:
        case RLE_MEGA_FG_RUN:
        case RLE_LITE_SET_FG_FG_RUN:
        case RLE_MEGA_SET_FG_RUN:
            if (rle_len(code, &c, &rl)) return -1;
            if (code == RLE_LITE_SET_FG_FG_RUN || code == RLE_MEGA_SET_FG_RUN)
                fg = rle_px(&c);
            if (c.bad || di + rl > total) return -1;
            if (first)
            {
                while (rl--) px[di++] = fg;
            }
            else
            {
                if (di < row) return -1;
                while (rl--) { px[di] = px[di - row] ^ fg; di++; }
            }
            break;

        case RLE_LITE_DITHERED_RUN:
        case RLE_MEGA_DITHERED_RUN:
        {
            uint32_t pa, pb2;
            if (rle_len(code, &c, &rl)) return -1;
            pa = rle_px(&c);
            pb2 = rle_px(&c);
            if (c.bad || di + rl * 2 > total) return -1;
            while (rl--) { px[di++] = pa; px[di++] = pb2; }
            break;
        }

        case RLE_COLOR_RUN:
        case RLE_MEGA_COLOR_RUN:
        {
            uint32_t pa;
            if (rle_len(code, &c, &rl)) return -1;
            pa = rle_px(&c);
            if (c.bad || di + rl > total) return -1;
            while (rl--) px[di++] = pa;
            break;
        }

        case RLE_FGBG_IMAGE:
        case RLE_MEGA_FGBG_IMAGE:
        case RLE_LITE_SET_FG_FGBG_IMAGE:
        case RLE_MEGA_SET_FGBG_IMAGE:
        {
            unsigned mask;
            int i;
            if (rle_len(code, &c, &rl)) return -1;
            if (code == RLE_LITE_SET_FG_FGBG_IMAGE || code == RLE_MEGA_SET_FGBG_IMAGE)
                fg = rle_px(&c);
            if (c.bad) return -1;
            while (rl > 8)
            {
                if (c.p >= c.n || di + 8 > total) return -1;
                if (!first && di < row) return -1;
                mask = c.s[c.p++];
                for (i = 0; i < 8; i++, di++)
                {
                    uint32_t up = first ? black : px[di - row];
                    px[di] = (mask & (1u << i)) ? (up ^ fg) : up;
                }
                rl -= 8;
            }
            if (rl)
            {
                if (c.p >= c.n || di + rl > total) return -1;
                if (!first && di < row) return -1;
                mask = c.s[c.p++];
                for (i = 0; (uint32_t)i < rl; i++, di++)
                {
                    uint32_t up = first ? black : px[di - row];
                    px[di] = (mask & (1u << i)) ? (up ^ fg) : up;
                }
            }
            break;
        }

        case RLE_COLOR_IMAGE:
        case RLE_MEGA_COLOR_IMAGE:
            if (rle_len(code, &c, &rl)) return -1;
            if (di + rl > total) return -1;
            while (rl--) { px[di++] = rle_px(&c); if (c.bad) return -1; }
            break;

        case RLE_SPECIAL_FGBG_1:
        case RLE_SPECIAL_FGBG_2:
        {
            unsigned mask = (code == RLE_SPECIAL_FGBG_1) ? 0x03u : 0x05u;
            int i;
            c.p++;                                  /* the byte is the whole order */
            if (di + 8 > total) return -1;
            if (!first && di < row) return -1;
            for (i = 0; i < 8; i++, di++)
            {
                uint32_t up = first ? black : px[di - row];
                px[di] = (mask & (1u << i)) ? (up ^ fg) : up;
            }
            break;
        }

        case RLE_SPECIAL_WHITE:
            c.p++;
            if (di >= total) return -1;
            px[di++] = white;
            break;

        case RLE_SPECIAL_BLACK:
            c.p++;
            if (di >= total) return -1;
            px[di++] = black;
            break;

        default:
            return -1;                  /* 0x05..0x0B, 0x0F..0xEF: reserved */
        }
    }

    if (c.bad) return -1;

    /* Stream row 0 is the bottom row of the bitmap (TS_BITMAP_DATA is
     * bottom-up), while blit() expects top-down rows - flip while
     * converting to RGB. */
    for (y = 0; y < h; y++)
    {
        uint8_t* d = rgb + (size_t)(h - 1 - y) * (size_t)w * 3;
        const uint32_t* s = px + (size_t)y * (size_t)w;
        for (x = 0; x < w; x++)
        {
            uint32_t v = s[x];
            uint8_t r, g, b;
            if (c.pb == 2)
            {
                if (bpp == 15)
                {
                    r = (uint8_t)(((v >> 10) & 0x1Fu) << 3);
                    g = (uint8_t)(((v >> 5) & 0x1Fu) << 3);
                    b = (uint8_t)((v & 0x1Fu) << 3);
                }
                else
                {
                    r = (uint8_t)(((v >> 11) & 0x1Fu) << 3);
                    g = (uint8_t)(((v >> 5) & 0x3Fu) << 2);
                    b = (uint8_t)((v & 0x1Fu) << 3);
                }
            }
            else if (c.pb == 1)
            {
                r = g = b = (uint8_t)v;
            }
            else
            {
                b = (uint8_t)(v & 0xFFu);
                g = (uint8_t)((v >> 8) & 0xFFu);
                r = (uint8_t)((v >> 16) & 0xFFu);
            }
            d[0] = r; d[1] = g; d[2] = b;
            d += 3;
        }
    }
    return 0;
}

int rdp_update_bitmap(const uint8_t* body, size_t len, rdp_surface* surface)
{
    unsigned nrect, i;
    size_t p = 4;
    int applied = 0;

    if (!body || !surface || len < 4) return -1;
    if ((body[0] | (body[1] << 8)) != 0x0001u) return -1;   /* UPDATETYPE_BITMAP */
    nrect = (unsigned)(body[2] | (body[3] << 8));

    for (i = 0; i < nrect; i++)
    {
        unsigned dl, dt, dr, db, w, h, bpp, flags, blen;

        if (p + 18 > len) break;
        dl = (unsigned)(body[p] | (body[p + 1] << 8));
        dt = (unsigned)(body[p + 2] | (body[p + 3] << 8));
        dr = (unsigned)(body[p + 4] | (body[p + 5] << 8));
        db = (unsigned)(body[p + 6] | (body[p + 7] << 8));
        w = (unsigned)(body[p + 8] | (body[p + 9] << 8));
        h = (unsigned)(body[p + 10] | (body[p + 11] << 8));
        bpp = (unsigned)(body[p + 12] | (body[p + 13] << 8));
        flags = (unsigned)(body[p + 14] | (body[p + 15] << 8));
        blen = (unsigned)(body[p + 16] | (body[p + 17] << 8));
        (void)dr; (void)db;
        p += 18;
        if ((flags & 0x0001u) && !(flags & 0x0400u))
        {
            /* TS_CD_HEADER present: cbCompFirstRowSize, cbCompMainBodySize,
             * cbScanWidth, cbUncompressedSize; bitmapLength is then taken
             * from cbCompMainBodySize. */
            unsigned mainbody;
            if (p + 8 > len) break;
            mainbody = (unsigned)(body[p + 2] | (body[p + 3] << 8));
            p += 8;
            if (mainbody) blen = mainbody;
        }
        if (p + blen > len) break;
        {
            static int rect_log = 0;
            extern int rdp_update_debug_log;
            if (rdp_update_debug_log && rect_log < 60)
            {
                rect_log++;
                ulog7("rect %d,%d %dx%d bpp=%d flags=0x%X blen=%d",
                     (int)dl, (int)dt, (int)w, (int)h, (int)bpp, (int)flags, (int)blen);
            }
        }
        if (w == 0 || h == 0 || w > TILE_MAX_W || h > TILE_MAX_H)
        {
            /* Bigger than the decode buffer: skipped.  Logged because a screen
             * that never paints usually means the server switched to
             * full-screen rectangles we cannot hold. */
            static int skipped = 0;
            extern int rdp_update_debug_log;
            if (rdp_update_debug_log && skipped < 3)
            {
                skipped++;
                ulog7("rect skip %d,%d %dx%d bpp=%d flags=0x%X blen=%d",
                      (int)dl, (int)dt, (int)w, (int)h, (int)bpp, (int)flags, (int)blen);
            }
            p += blen;
            continue;
        }

        if (flags & 0x0002u)
        {
            /* BITMAP_COMPRESSION: interleaved RLE, not implemented.  Log once so
             * a black screen is diagnosable instead of mysterious. */
            static int rle_warned = 0;
            if (rle_warned < 4)
            {
                extern int rdp_update_debug_log;
                rle_warned++;
                (void)rdp_update_debug_log;
            }
        }

        if (flags & 0x0001u)
        {
            /* Two different encodings share BITMAP_COMPRESSION (0x0001):
             *   - RDP 6 "planar": fmt byte + [alpha] Y Co Cg planes, 24/32bpp.
             *     This is what Windows 8/10/11 send, and it is also what a
             *     Windows 10 server sends with NO_BITMAP_COMPRESSION_HDR set
             *     (flags 0x0401) - there is no legacy RLE involved.
             *   - RDP 5 interleaved RLE: colour/run orders, 8/15/16bpp.
             *     Windows 7 / 2008 R2 era servers and Server 2012 in RDP 5
             *     mode send this.
             * The pixel depth picks the likely one and the other decoder is
             * used as a fallback, so a 32bpp planar stream can never be fed
             * to the RLE decoder again (that was the "grey screen on the
             * Windows 10 server" bug). */
            const uint8_t* src = body + p;
            static int n_planar, n_rle;
            int ok;

            if (bpp == 32 || bpp == 24)
            {
                ok = (rdp_planar_decode(src, blen, (int)w, (int)h, s_tile) == 0);
                if (ok) n_planar++;
                else if ((ok = (rle_decompress(src, blen, (int)w, (int)h, (int)bpp,
                                               s_tile) == 0)) != 0) n_rle++;
            }
            else
            {
                ok = (rle_decompress(src, blen, (int)w, (int)h, (int)bpp, s_tile) == 0);
                if (ok) n_rle++;
                else if ((ok = (rdp_planar_decode(src, blen, (int)w, (int)h,
                                                 s_tile) == 0)) != 0) n_planar++;
            }

            if (ok)
            {
                if (rdp_update_debug_log)
                {
                    /* First few *distinct* payloads, so a bad decode can be
                     * checked against the bytes that produced it. */
                    static uint8_t last[64];
                    static int lastlen, dumps;
                    if (dumps < 4 &&
                        (blen > sizeof(last) || blen != (size_t)lastlen ||
                         memcmp(last, src, blen) != 0))
                    {
                        char hex[200], line[120];
                        int o = 0, n2 = blen < 40 ? (int)blen : 40;
                        dumps++;
                        lastlen = blen <= sizeof(last) ? (int)blen : 0;
                        if (lastlen) memcpy(last, src, (size_t)lastlen);
                        for (int k = 0; k < n2 && o < 170; k++)
                            o += snprintf(hex + o, sizeof(hex) - o, "%02x", src[k]);
                        snprintf(line, sizeof(line), "bmp dump %d,%d %dx%d bpp=%d len=%d fmt=%02X",
                                 (int)dl, (int)dt, (int)w, (int)h, (int)bpp, (int)blen,
                                 (unsigned)src[0]);
                        if (rdp_update_log_hook) rdp_update_log_hook(line);
                        if (rdp_update_log_hook) rdp_update_log_hook(hex);
                    }
                    if (n_planar + n_rle <= 6)
                        ulog7("bmp ok %d,%d %dx%d bpp=%d len=%d planar=%d",
                              (int)dl, (int)dt, (int)w, (int)h, (int)bpp, (int)blen, n_planar);
                }
                blit(surface, (int)dl, (int)dt, (int)w, (int)h, s_tile);
                applied++;
            }
            else
            {
                static int fails = 0;
                if (rdp_update_debug_log && fails < 8)
                {
                    fails++;
                    ulog7("bmp fail %d,%d %dx%d bpp=%d len=%d fmt=%02X",
                          (int)dl, (int)dt, (int)w, (int)h, (int)bpp, (int)blen,
                          (unsigned)src[0]);
                }
            }
        }
        else
        {
            blit_raw(surface, (int)dl, (int)dt, (int)w, (int)h, (int)bpp, body + p, blen);
            applied++;
        }
        p += blen;
    }
    return applied;
}

int rdp_update_fastpath(const uint8_t* payload, size_t len,
                        rdp_surface* surface,
                        uint8_t* frag_buf, size_t frag_cap,
                        size_t* frag_len, int* frag_active)
{
    size_t p = 0;
    int applied = 0;

    while (p + 3 <= len)
    {
        unsigned hdr = payload[p++];
        unsigned code = hdr & 0x0F;
        unsigned frag = (hdr >> 4) & 0x03;
        unsigned comp = (hdr >> 6) & 0x03;
        unsigned size;
        const uint8_t* data;

        if (comp != 0)
        {
            /* FASTPATH_OUTPUT_COMPRESSION_USED: one extra flags byte */
            if (p >= len) return applied;
            p++;
        }
        if (p + 2 > len) return applied;
        size = (unsigned)(payload[p] | (payload[p + 1] << 8));
        p += 2;
        if (p + size > len) return applied;
        data = payload + p;
        p += size;

        if (code != UPD_CODE_BITMAP)
        {
            extern int rdp_update_other_codes[16];
            rdp_update_other_codes[code]++;
            continue;
        }

        if (frag == UPD_FRAG_SINGLE)
        {
            applied += rdp_update_bitmap(data, size, surface);
            continue;
        }
        if (frag == UPD_FRAG_FIRST)
        {
            if (size <= frag_cap)
            {
                memcpy(frag_buf, data, size);
                *frag_len = size;
                *frag_active = 1;
            }
            else
            {
                *frag_active = 0;
                *frag_len = 0;
            }
            continue;
        }
        if (frag == UPD_FRAG_NEXT)
        {
            if (*frag_active && *frag_len + size <= frag_cap)
            {
                memcpy(frag_buf + *frag_len, data, size);
                *frag_len += size;
            }
            else
            {
                *frag_active = 0;
                *frag_len = 0;
            }
            continue;
        }
        /* LAST */
        if (*frag_active && *frag_len + size <= frag_cap)
        {
            memcpy(frag_buf + *frag_len, data, size);
            *frag_len += size;
            applied += rdp_update_bitmap(frag_buf, *frag_len, surface);
        }
        *frag_active = 0;
        *frag_len = 0;
    }
    return applied;
}
