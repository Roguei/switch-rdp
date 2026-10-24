/*
 * rdp_bitmap.c - decode RDP bitmap updates into RGBA8888.
 *
 * The primary wire format handled here is the "bitmapData" block used by
 * MS-RDPBCGR.  In addition to the explicit compression field used by this
 * project's update format, the native TS_BITMAP_DATA layout (flags followed
 * by bitmapLength) is accepted as a fallback.  Both compressed and
 * uncompressed rectangles can be present in one update.
 *
 * Pixels are emitted as bytes R,G,B,A, i.e. the little-endian 32-bit value
 * 0xAABBGGRR.  Bitmap rows are bottom-up: decoded source row zero is placed
 * at destBottom.  All writes are clipped to the supplied surface.
 */

#include "rdp_bitmap.h"

#include <stdlib.h>
#include <string.h>

#define RDP_UPDATETYPE_BITMAP       0x0001u
#define RDP_BITMAP_COMPRESSION      0x0001u
#define RDP_NO_COMPRESSION_HDR      0x0400u
#define RDP_BITMAP_FLAG_MASK        0x0401u

#define RDP_BI_RGB                  0
#define RDP_BI_RLE8                 1
#define RDP_BI_RLE4                 2

/* Layouts tried for a bitmapData block.  The first is the format documented
 * by this module's interface; the others are tolerant fallbacks for native
 * RDP bitmap updates and for streams whose length is implied by the end of
 * the update. */
typedef enum
{
    RDP_LAYOUT_PROMPT_LEN = 0,
    RDP_LAYOUT_STANDARD_LEN,
    RDP_LAYOUT_STANDARD_BODY,
    RDP_LAYOUT_PROMPT_REST
} rdp_layout;

typedef struct
{
    int             left;
    int             top;
    int             right;
    int             bottom;
    int             width;
    int             height;
    int             bpp;
    int             compression;
    unsigned        flags;
    const uint8_t*  stream;
    size_t          stream_len;
} rdp_bitmap_rect;

static uint16_t rdp_read_u16(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int rdp_bpp_supported(int bpp)
{
    return (bpp == 1 || bpp == 4 || bpp == 8 || bpp == 15 ||
            bpp == 16 || bpp == 24 || bpp == 32);
}

static size_t rdp_raw_stride(int width, int bpp)
{
    /* 15-bpp pixels still occupy 16 bits on the wire. */
    uint32_t stored_bpp = (bpp == 15) ? 16u : (uint32_t)bpp;
    uint64_t bits = (uint64_t)(uint32_t)width * stored_bpp;
    return (size_t)(((bits + 31u) / 32u) * 4u);
}

static void rdp_store_pixel(uint8_t* dst, uint8_t r, uint8_t g, uint8_t b)
{
    dst[0] = r;
    dst[1] = g;
    dst[2] = b;
    dst[3] = 0xFF;
}

static uint8_t rdp_expand5(unsigned int value)
{
    value &= 0x1Fu;
    return (uint8_t)((value << 3) | (value >> 2));
}

/* Parse one bitmap rectangle header at the given offset.  The parser only
 * locates the data stream; rdp_rect_valid() checks whether the stream is
 * large enough for the selected pixel format. */
static int rdp_parse_rect(const uint8_t* data, size_t len, size_t offset,
                          rdp_layout layout, rdp_bitmap_rect* rect,
                          size_t* next_offset)
{
    uint16_t left, top, right, bottom;
    uint16_t width, height, bpp;
    uint16_t field7, field8;
    uint16_t flags = 0;
    uint16_t declared_len = 0;
    uint16_t body_len = 0;
    uint16_t stream_len16 = 0;
    int compression = 0;
    size_t pos;

    if (!data || !rect || !next_offset)
        return -1;
    if (offset > len || len - offset < 18)
        return -1;

    left   = rdp_read_u16(data + offset + 0);
    top    = rdp_read_u16(data + offset + 2);
    right  = rdp_read_u16(data + offset + 4);
    bottom = rdp_read_u16(data + offset + 6);
    width  = rdp_read_u16(data + offset + 8);
    height = rdp_read_u16(data + offset + 10);
    bpp    = rdp_read_u16(data + offset + 12);
    field7 = rdp_read_u16(data + offset + 14);
    field8 = rdp_read_u16(data + offset + 16);

    if (width == 0 || height == 0)
        return -1;
    if (right < left || bottom < top)
        return -1;
    if (!rdp_bpp_supported((int)bpp))
        return -1;

    pos = offset + 18;

    if (layout == RDP_LAYOUT_PROMPT_LEN || layout == RDP_LAYOUT_PROMPT_REST)
    {
        compression = (int)field7;
        flags = field8;

        if (compression < RDP_BI_RGB || compression > RDP_BI_RLE4)
            return -1;
        if ((flags & (uint16_t)~RDP_BITMAP_FLAG_MASK) != 0)
            return -1;

        if (compression != RDP_BI_RGB &&
            (flags & RDP_BITMAP_COMPRESSION) != 0 &&
            (flags & RDP_NO_COMPRESSION_HDR) == 0)
        {
            if (len - pos < 8)
                return -1;
            pos += 8;
        }

        if (layout == RDP_LAYOUT_PROMPT_LEN)
        {
            if (len - pos < 2)
                return -1;
            stream_len16 = rdp_read_u16(data + pos);
            pos += 2;
            if ((size_t)stream_len16 > len - pos)
                return -1;
            rect->stream = data + pos;
            rect->stream_len = (size_t)stream_len16;
            pos += (size_t)stream_len16;
        }
        else
        {
            rect->stream = data + pos;
            rect->stream_len = len - pos;
            pos = len;
        }
    }
    else
    {
        flags = field7;
        declared_len = field8;

        if ((flags & (uint16_t)~RDP_BITMAP_FLAG_MASK) != 0)
            return -1;

        if ((flags & RDP_BITMAP_COMPRESSION) != 0)
        {
            /* In native bitmap data the compression scheme follows from the
             * bitmap depth; there is no separate BI_xxx field. */
            if (bpp == 8)
                compression = RDP_BI_RLE8;
            else if (bpp == 4)
                compression = RDP_BI_RLE4;
            else
                return -1;

            if ((flags & RDP_NO_COMPRESSION_HDR) == 0)
            {
                if (len - pos < 8)
                    return -1;
                body_len = rdp_read_u16(data + pos + 2);
                pos += 8;
                stream_len16 = (layout == RDP_LAYOUT_STANDARD_BODY)
                                   ? body_len
                                   : declared_len;
            }
            else
            {
                stream_len16 = declared_len;
            }
        }
        else
        {
            compression = RDP_BI_RGB;
            stream_len16 = declared_len;
        }

        if ((size_t)stream_len16 > len - pos)
            return -1;
        rect->stream = data + pos;
        rect->stream_len = (size_t)stream_len16;
        pos += (size_t)stream_len16;
    }

    rect->left = (int)left;
    rect->top = (int)top;
    rect->right = (int)right;
    rect->bottom = (int)bottom;
    rect->width = (int)width;
    rect->height = (int)height;
    rect->bpp = (int)bpp;
    rect->compression = compression;
    rect->flags = flags;

    *next_offset = pos;
    return 0;
}

static int rdp_decodable_rle(const rdp_bitmap_rect* rect, int require_eob);

static int rdp_rect_valid(const rdp_bitmap_rect* rect)
{
    size_t stride;

    if (!rect || rect->width <= 0 || rect->height <= 0)
        return 0;
    if (!rdp_bpp_supported(rect->bpp))
        return 0;

    if (rect->compression == RDP_BI_RLE8)
        return rect->bpp == 8 && rect->stream_len > 0;
    if (rect->compression == RDP_BI_RLE4)
        return rect->bpp == 4 && rect->stream_len > 0;
    if (rect->compression != RDP_BI_RGB)
        return 0;

    stride = rdp_raw_stride(rect->width, rect->bpp);
    if (stride == 0 || rect->stream_len < stride)
        return 0;
    if ((size_t)(unsigned)rect->height > rect->stream_len / stride)
        return 0;

    return 1;
}

/* The "rest of the payload" variant has no explicit stream length.  Only
 * accept it for compressed data with NO_BITMAP_COMPRESSION_HDR, and only when
 * the RLE stream is complete and ends with an end-of-bitmap marker.  Raw
 * bitmaps are intentionally excluded: a length-prefixed raw stream that lost
 * two trailing bytes would otherwise look exactly like a valid raw stream
 * with its first two pixel bytes interpreted as a length. */
static int rdp_rest_rect_valid(const rdp_bitmap_rect* rect)
{
    if (rect->compression == RDP_BI_RGB)
        return 0;
    if ((rect->flags & RDP_NO_COMPRESSION_HDR) == 0)
        return 0;

    return rdp_decodable_rle(rect, 1);
}

static int rdp_validate_layout(const uint8_t* data, size_t len, int count,
                               rdp_layout layout, size_t* consumed)
{
    size_t pos = 4;
    int i;

    if (layout == RDP_LAYOUT_PROMPT_REST && count != 1)
        return -1;

    for (i = 0; i < count; ++i)
    {
        rdp_bitmap_rect rect;
        size_t next;

        if (rdp_parse_rect(data, len, pos, layout, &rect, &next) != 0)
            return -1;
        if (!rdp_rect_valid(&rect))
            return -1;
        if (layout == RDP_LAYOUT_PROMPT_REST &&
            !rdp_rest_rect_valid(&rect))
            return -1;
        pos = next;
    }

    if (consumed)
        *consumed = pos;
    return 0;
}

static int rdp_select_layout(const uint8_t* data, size_t len, int count)
{
    size_t end;
    int best = -1;
    int best_score = -1;
    int score;

    if (rdp_validate_layout(data, len, count, RDP_LAYOUT_PROMPT_LEN,
                            &end) == 0)
    {
        score = (end == len ? 1000 : 0) + 4;
        if (score > best_score)
        {
            best_score = score;
            best = RDP_LAYOUT_PROMPT_LEN;
        }
    }

    if (rdp_validate_layout(data, len, count, RDP_LAYOUT_STANDARD_LEN,
                            &end) == 0)
    {
        score = (end == len ? 1000 : 0) + 2;
        if (score > best_score)
        {
            best_score = score;
            best = RDP_LAYOUT_STANDARD_LEN;
        }
    }

    if (rdp_validate_layout(data, len, count, RDP_LAYOUT_STANDARD_BODY,
                            &end) == 0)
    {
        score = (end == len ? 1000 : 0) + 1;
        if (score > best_score)
        {
            best_score = score;
            best = RDP_LAYOUT_STANDARD_BODY;
        }
    }

    if (best >= 0)
        return best;

    if (rdp_validate_layout(data, len, count, RDP_LAYOUT_PROMPT_REST,
                            &end) == 0)
        return RDP_LAYOUT_PROMPT_REST;

    return -1;
}

static int rdp_decode_raw(const rdp_bitmap_rect* rect, rdp_surface* surface)
{
    size_t stride;
    int first_row = 0;
    int last_row = rect->height - 1;
    int last_x = rect->width - 1;
    int row_min;
    int sy;

    if (surface->width <= 0 || surface->height <= 0)
        return 0;
    if (rect->left >= surface->width)
        return 0;

    /* last_x is an index into the source row; only the part that lands on
     * the surface has to be converted (and read). */
    if (last_x >= surface->width - rect->left)
        last_x = surface->width - rect->left - 1;
    if (last_x < 0)
        return 0;

    /* dest_y = bottom - sy must be in [0, surface->height - 1]. */
    if (rect->bottom < 0)
        return 0;
    if (last_row > rect->bottom)
        last_row = rect->bottom;
    row_min = rect->bottom - (surface->height - 1);
    if (row_min > first_row)
        first_row = row_min;
    if (first_row > last_row)
        return 0;

    stride = rdp_raw_stride(rect->width, rect->bpp);

    for (sy = first_row; sy <= last_row; ++sy)
    {
        const uint8_t* row = rect->stream + (size_t)sy * stride;
        int dy = rect->bottom - sy;
        uint8_t* dst = surface->pixels +
                       ((size_t)dy * (size_t)surface->width +
                        (size_t)rect->left) * 4;
        int x;

        switch (rect->bpp)
        {
            case 1:
                for (x = 0; x <= last_x; ++x)
                {
                    uint8_t value = row[x >> 3];
                    uint8_t index = (uint8_t)
                        ((value >> (7 - (x & 7))) & 0x01u);
                    rdp_store_pixel(dst + (size_t)x * 4,
                                    index, index, index);
                }
                break;

            case 4:
                for (x = 0; x <= last_x; ++x)
                {
                    uint8_t value = row[x >> 1];
                    uint8_t index = (uint8_t)
                        ((x & 1) ? (value & 0x0Fu) : (value >> 4));
                    rdp_store_pixel(dst + (size_t)x * 4,
                                    index, index, index);
                }
                break;

            case 8:
                for (x = 0; x <= last_x; ++x)
                {
                    uint8_t index = row[x];
                    rdp_store_pixel(dst + (size_t)x * 4,
                                    index, index, index);
                }
                break;

            case 15:
            case 16:
                for (x = 0; x <= last_x; ++x)
                {
                    const uint8_t* src = row + (size_t)x * 2;
                    unsigned int value =
                        (unsigned int)src[0] |
                        ((unsigned int)src[1] << 8);
                    uint8_t r = rdp_expand5(value >> 10);
                    uint8_t g = rdp_expand5(value >> 5);
                    uint8_t b = rdp_expand5(value);
                    rdp_store_pixel(dst + (size_t)x * 4, r, g, b);
                }
                break;

            case 24:
                for (x = 0; x <= last_x; ++x)
                {
                    const uint8_t* src = row + (size_t)x * 3;
                    rdp_store_pixel(dst + (size_t)x * 4,
                                    src[2], src[1], src[0]);
                }
                break;

            case 32:
                for (x = 0; x <= last_x; ++x)
                {
                    const uint8_t* src = row + (size_t)x * 4;
                    rdp_store_pixel(dst + (size_t)x * 4,
                                    src[2], src[1], src[0]);
                }
                break;

            default:
                return -1;
        }
    }

    return 0;
}

/* Write one decoded indexed scanline.  The source line is bottom-up, so
 * source row 0 goes to destBottom. */
static void rdp_blit_index_line(const uint8_t* line, int width,
                                const rdp_bitmap_rect* rect, int source_row,
                                rdp_surface* surface)
{
    int dy = rect->bottom - source_row;
    int count = width;
    int x;
    uint8_t* dst;

    if ((unsigned int)dy >= (unsigned int)surface->height)
        return;
    if (surface->width <= 0 || rect->left >= surface->width)
        return;
    if (count > surface->width - rect->left)
        count = surface->width - rect->left;
    if (count <= 0)
        return;

    dst = surface->pixels +
          ((size_t)dy * (size_t)surface->width + (size_t)rect->left) * 4;

    for (x = 0; x < count; ++x)
    {
        uint8_t index = line[x];
        rdp_store_pixel(dst + (size_t)x * 4, index, index, index);
    }
}

static int rdp_decode_rle(const rdp_bitmap_rect* rect, rdp_surface* surface,
                          int require_eob)
{
    uint8_t* line;
    const uint8_t* p;
    const uint8_t* end;
    int x = 0;
    int row = 0;
    int dirty = 0;
    int is_rle4 = (rect->compression == RDP_BI_RLE4);

    line = (uint8_t*)malloc((size_t)rect->width);
    if (!line)
        return -1;
    memset(line, 0, (size_t)rect->width);

    p = rect->stream;
    end = p + rect->stream_len;

    while (p < end)
    {
        uint8_t control = *p++;

        if (control != 0)
        {
            uint8_t value;

            if (p >= end)
                goto malformed;
            value = *p++;

            if (row >= rect->height || (int)control > rect->width - x)
                goto malformed;

            if (is_rle4)
            {
                int i;
                for (i = 0; i < (int)control; ++i)
                {
                    line[x + i] = (uint8_t)
                        ((i & 1) ? (value & 0x0Fu) : (value >> 4));
                }
            }
            else
            {
                memset(line + x, (int)value, (size_t)control);
            }

            x += (int)control;
            dirty = 1;
        }
        else
        {
            uint8_t code;

            if (p >= end)
                goto malformed;
            code = *p++;

            if (code == 0)
            {
                /* End of line. */
                if (surface && dirty)
                    rdp_blit_index_line(line, rect->width, rect, row,
                                        surface);
                memset(line, 0, (size_t)rect->width);
                x = 0;
                dirty = 0;
                ++row;
                if (row > rect->height)
                    goto malformed;
            }
            else if (code == 1)
            {
                /* End of bitmap. */
                if (surface && dirty)
                    rdp_blit_index_line(line, rect->width, rect, row,
                                        surface);
                free(line);
                return 0;
            }
            else if (code == 2)
            {
                /* Delta: move the cursor without painting. */
                int dx;
                int dy;

                if ((size_t)(end - p) < 2)
                    goto malformed;
                dx = (int)*p++;
                dy = (int)*p++;

                if (dx > rect->width - x || row + dy > rect->height)
                    goto malformed;

                if (dy != 0)
                {
                    if (surface && dirty)
                    {
                        rdp_blit_index_line(line, rect->width, rect, row,
                                            surface);
                        dirty = 0;
                    }
                    memset(line, 0, (size_t)rect->width);
                    row += dy;
                }
                x += dx;
            }
            else
            {
                /* Absolute run.  RLE8 stores one byte per pixel; RLE4 packs
                 * two pixels per byte.  Either kind is padded to a 4-byte
                 * boundary before the next command. */
                int n = (int)code;
                size_t need;
                int i;

                if (row >= rect->height || n > rect->width - x)
                    goto malformed;

                if (is_rle4)
                {
                    size_t bytes = ((size_t)n + 1u) / 2u;
                    need = bytes + ((4u - (bytes & 3u)) & 3u);
                    if (need > (size_t)(end - p))
                        goto malformed;

                    for (i = 0; i < n; ++i)
                    {
                        uint8_t value = p[(size_t)i >> 1];
                        line[x + i] = (uint8_t)
                            ((i & 1) ? (value & 0x0Fu) : (value >> 4));
                    }
                }
                else
                {
                    need = (size_t)n + ((4u - ((size_t)n & 3u)) & 3u);
                    if (need > (size_t)(end - p))
                        goto malformed;

                    for (i = 0; i < n; ++i)
                        line[x + i] = p[i];
                }

                p += need;
                x += n;
                dirty = 1;
            }
        }
    }

    if (require_eob)
        goto malformed;

    /* A stream may end after an end-of-line instead of an end-of-bitmap, but
     * it must not stop in the middle of a scanline. */
    if (dirty)
        goto malformed;

    free(line);
    return 0;

malformed:
    free(line);
    return -1;
}

static int rdp_decodable_rle(const rdp_bitmap_rect* rect, int require_eob)
{
    return rdp_decode_rle(rect, NULL, require_eob) == 0;
}

int rdp_bitmap_process(const uint8_t* data, size_t len, rdp_surface* surface)
{
    int count;
    int layout;
    size_t pos;
    int i;

    if (!data || !surface || !surface->pixels)
        return -1;
    if (surface->width < 0 || surface->height < 0)
        return -1;
    if (len < 4)
        return -1;
    if (rdp_read_u16(data) != RDP_UPDATETYPE_BITMAP)
        return -1;

    count = (int)rdp_read_u16(data + 2);
    if ((size_t)count > (len - 4) / 18)
        return -1;
    if (count == 0)
        return 0;

    layout = rdp_select_layout(data, len, count);
    if (layout < 0)
        return -1;

    pos = 4;
    for (i = 0; i < count; ++i)
    {
        rdp_bitmap_rect rect;
        size_t next;
        int rc;

        if (rdp_parse_rect(data, len, pos, (rdp_layout)layout,
                           &rect, &next) != 0)
            return -1;
        if (!rdp_rect_valid(&rect))
            return -1;

        if (rect.compression == RDP_BI_RGB)
            rc = rdp_decode_raw(&rect, surface);
        else
            rc = rdp_decode_rle(&rect, surface, 0);

        if (rc != 0)
            return -1;
        pos = next;
    }

    return 0;
}
