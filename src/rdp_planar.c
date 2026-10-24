/*
 * rdp_planar.c - RDP6 Planar bitmap decoder.
 *
 * See rdp_planar.h for the stream layout.  Bounded for the tile sizes the
 * server actually sends (at most 64x64); nothing here allocates.
 */

#include "rdp_planar.h"

#include <string.h>

#define PLANAR_HEADER_CLL_MASK 0x07
#define PLANAR_HEADER_CS       0x08
#define PLANAR_HEADER_RLE      0x10
#define PLANAR_HEADER_NA       0x20

#define PLANAR_MAX_W 128
#define PLANAR_MAX_H 128
#define PLANAR_MAX_PIXELS (PLANAR_MAX_W * PLANAR_MAX_H)

/* one plane, at most PLANAR_MAX_PIXELS bytes */
static uint8_t s_plane[4][PLANAR_MAX_PIXELS];
static uint8_t s_chroma[2][PLANAR_MAX_PIXELS];

static int clampi(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

/*
 * Decodes one RLE plane.  `src`, `len` is the remaining stream; `out` is
 * w*h bytes.  Rows are delta coded against the previous row: the first row
 * holds absolute values, every later row holds signed deltas.
 * Returns the number of source bytes consumed, or -1.
 */
static int rle_plane(const uint8_t* src, size_t len, uint8_t* out, int w, int h)
{
    size_t p = 0;
    int y;

    for (y = 0; y < h; y++)
    {
        uint8_t* row = out + (size_t)y * (size_t)w;
        const uint8_t* prev = (y > 0) ? row - w : NULL;
        int x = 0;
        int last = 0;

        while (x < w)
        {
            int cb, run, raw, k;

            if (p >= len) return -1;
            cb = src[p++];
            run = cb & 0x0F;
            raw = (cb >> 4) & 0x0F;

            if (run == 1)
            {
                run = raw + 16;
                raw = 0;
            }
            else if (run == 2)
            {
                run = raw + 32;
                raw = 0;
            }

            if (x + raw + run > w) return -1;
            if (p + (size_t)raw > len) return -1;

            if (!prev)
            {
                for (k = 0; k < raw; k++)
                {
                    last = src[p++];
                    row[x++] = (uint8_t)last;
                }
                for (k = 0; k < run; k++)
                    row[x++] = (uint8_t)last;
            }
            else
            {
                for (k = 0; k < raw; k++)
                {
                    int dv = src[p++];
                    if (dv & 1)
                        last = -((dv >> 1) + 1);
                    else
                        last = dv >> 1;
                    row[x] = (uint8_t)(prev[x] + last);
                    x++;
                }
                for (k = 0; k < run; k++)
                {
                    row[x] = (uint8_t)(prev[x] + last);
                    x++;
                }
            }
        }
    }

    return (int)p;
}

int rdp_planar_decode(const uint8_t* data, size_t len, int w, int h, uint8_t* rgb)
{
    int fmt, cll, cs, rle, alpha;
    int subw, subh, plane, sub;
    int pw[4], ph[4], sz[4];
    int have[4];
    size_t p = 0;
    int order[4], norder = 0, i, x, y;
    int shift;

    if (!data || !rgb || len < 1) return -1;
    if (w <= 0 || h <= 0 || w > PLANAR_MAX_W || h > PLANAR_MAX_H) return -1;

    fmt = data[0];
    cll = fmt & PLANAR_HEADER_CLL_MASK;
    cs = (fmt & PLANAR_HEADER_CS) ? 1 : 0;
    rle = (fmt & PLANAR_HEADER_RLE) ? 1 : 0;
    alpha = (fmt & PLANAR_HEADER_NA) ? 0 : 1;

    /* Chroma subsampling requires YCoCg (CLL != 0). */
    if (!cll && cs) return -1;

    subw = (w + 1) / 2;
    subh = (h + 1) / 2;
    plane = w * h;
    sub = subw * subh;

    if (cs)
    {
        pw[0] = w;    ph[0] = h;    sz[0] = plane;
        pw[1] = subw; ph[1] = subh; sz[1] = sub;
        pw[2] = subw; ph[2] = subh; sz[2] = sub;
        pw[3] = w;    ph[3] = h;    sz[3] = plane;
    }
    else
    {
        for (i = 0; i < 4; i++)
        {
            pw[i] = w;
            ph[i] = h;
            sz[i] = plane;
        }
    }

    have[0] = have[1] = have[2] = have[3] = 0;
    p = 1;

    if (alpha) order[norder++] = 3;
    order[norder++] = 0;
    order[norder++] = 1;
    order[norder++] = 2;

    for (i = 0; i < norder; i++)
    {
        int idx = order[i];
        if (rle)
        {
            int used = rle_plane(data + p, len - p, s_plane[idx], pw[idx], ph[idx]);
            if (used < 0) return -1;
            p += (size_t)used;
        }
        else
        {
            size_t need = (size_t)pw[idx] * (size_t)ph[idx];
            if (p + need > len) return -1;
            memcpy(s_plane[idx], data + p, need);
            p += need;
        }
        have[idx] = 1;
    }
    (void)sz;
    (void)have;

    if (cll == 0)
    {
        /* planes are plain R, G, B */
        const uint8_t* R = s_plane[0];
        const uint8_t* G = s_plane[1];
        const uint8_t* B = s_plane[2];
        for (y = 0; y < h; y++)
        {
            const uint8_t* rr = R + (size_t)(h - 1 - y) * (size_t)w;
            const uint8_t* gg = G + (size_t)(h - 1 - y) * (size_t)w;
            const uint8_t* bb = B + (size_t)(h - 1 - y) * (size_t)w;
            uint8_t* d = rgb + (size_t)y * (size_t)w * 3;
            for (x = 0; x < w; x++)
            {
                d[0] = rr[x];
                d[1] = gg[x];
                d[2] = bb[x];
                d += 3;
            }
        }
        return 0;
    }

    /* YCoCg */
    {
        const uint8_t* Y = s_plane[0];
        const uint8_t* Co;
        const uint8_t* Cg;

        if (cs)
        {
            /* 2x2 expand both chroma planes */
            for (i = 0; i < 2; i++)
            {
                const uint8_t* src = s_plane[1 + i];
                uint8_t* dst = s_chroma[i];
                for (y = 0; y < h; y++)
                {
                    const uint8_t* srow = src + (size_t)(y / 2) * (size_t)subw;
                    uint8_t* drow = dst + (size_t)y * (size_t)w;
                    for (x = 0; x < w; x++)
                        drow[x] = srow[x / 2];
                }
            }
            Co = s_chroma[0];
            Cg = s_chroma[1];
        }
        else
        {
            Co = s_plane[1];
            Cg = s_plane[2];
        }

        shift = cll - 1;
        for (y = 0; y < h; y++)
        {
            const uint8_t* yr = Y + (size_t)(h - 1 - y) * (size_t)w;
            const uint8_t* cor = Co + (size_t)(h - 1 - y) * (size_t)w;
            const uint8_t* cgr = Cg + (size_t)(h - 1 - y) * (size_t)w;
            uint8_t* d = rgb + (size_t)y * (size_t)w * 3;
            for (x = 0; x < w; x++)
            {
                int cg = (int)(int8_t)(uint8_t)(cgr[x] << shift);
                int co = (int)(int8_t)(uint8_t)(cor[x] << shift);
                int yy = yr[x];
                int t = yy - cg;
                d[0] = (uint8_t)clampi(t - co);
                d[1] = (uint8_t)clampi(yy + cg);
                d[2] = (uint8_t)clampi(t + co);
                d += 3;
            }
        }
    }

    return 0;
}
