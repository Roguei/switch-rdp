"""Simulate FreeRDP's interleaved-RLE decoder against captured / hand-built
order streams.

Second, independent implementation of the algorithm in
libfreerdp/codec/include/bitmap.h (RLEDECOMPRESS): it is used for
  * identifying what Windows Server 2012 actually sends (see docs
    04_TESTING), and
  * cross-checking the C decoder in src/rdp_update.c via
    tools/_rle_test.exe + tools/_rle_sim_batch.py (same FNV hash).
"""
import sys

REGULAR_BG_RUN = 0x00
MEGA_MEGA_BG_RUN = 0xF0
REGULAR_FG_RUN = 0x01
MEGA_MEGA_FG_RUN = 0xF1
LITE_SET_FG_FG_RUN = 0x0C
MEGA_MEGA_SET_FG_RUN = 0xF6
LITE_DITHERED_RUN = 0x0E
MEGA_MEGA_DITHERED_RUN = 0xF8
REGULAR_COLOR_RUN = 0x03
MEGA_MEGA_COLOR_RUN = 0xF3
REGULAR_FGBG_IMAGE = 0x02
MEGA_MEGA_FGBG_IMAGE = 0xF2
LITE_SET_FG_FGBG_IMAGE = 0x0D
MEGA_MEGA_SET_FGBG_IMAGE = 0xF7
REGULAR_COLOR_IMAGE = 0x04
MEGA_MEGA_COLOR_IMAGE = 0xF4
SPECIAL_FGBG_1 = 0xF9
SPECIAL_FGBG_2 = 0xFA
SPECIAL_WHITE = 0xFD
SPECIAL_BLACK = 0xFE

NAMES = {
    REGULAR_BG_RUN: "REGULAR_BG_RUN", MEGA_MEGA_BG_RUN: "MEGA_MEGA_BG_RUN",
    REGULAR_FG_RUN: "REGULAR_FG_RUN", MEGA_MEGA_FG_RUN: "MEGA_MEGA_FG_RUN",
    LITE_SET_FG_FG_RUN: "LITE_SET_FG_FG_RUN", MEGA_MEGA_SET_FG_RUN: "MEGA_MEGA_SET_FG_RUN",
    LITE_DITHERED_RUN: "LITE_DITHERED_RUN", MEGA_MEGA_DITHERED_RUN: "MEGA_MEGA_DITHERED_RUN",
    REGULAR_COLOR_RUN: "REGULAR_COLOR_RUN", MEGA_MEGA_COLOR_RUN: "MEGA_MEGA_COLOR_RUN",
    REGULAR_FGBG_IMAGE: "REGULAR_FGBG_IMAGE", MEGA_MEGA_FGBG_IMAGE: "MEGA_MEGA_FGBG_IMAGE",
    LITE_SET_FG_FGBG_IMAGE: "LITE_SET_FG_FGBG_IMAGE",
    MEGA_MEGA_SET_FGBG_IMAGE: "MEGA_MEGA_SET_FGBG_IMAGE",
    REGULAR_COLOR_IMAGE: "REGULAR_COLOR_IMAGE", MEGA_MEGA_COLOR_IMAGE: "MEGA_MEGA_COLOR_IMAGE",
    SPECIAL_FGBG_1: "SPECIAL_FGBG_1", SPECIAL_FGBG_2: "SPECIAL_FGBG_2",
    SPECIAL_WHITE: "SPECIAL_WHITE", SPECIAL_BLACK: "SPECIAL_BLACK",
}


class Fail(Exception):
    pass


def code_id(b):
    if (b & 0xC0) != 0xC0:
        return b >> 5
    if (b & 0xF0) == 0xF0:
        return b
    return b >> 4


def read_px(src, p, pb):
    if p + pb > len(src):
        raise Fail("pixel out of range")
    return int.from_bytes(src[p:p + pb], "little"), p + pb


def run_length(code, src, p):
    """(run length, index of first byte after the order header)."""
    hdr = src[p]
    q = p + 1
    if code in (REGULAR_FGBG_IMAGE, LITE_SET_FG_FGBG_IMAGE):
        rl = hdr & (0x1F if code == REGULAR_FGBG_IMAGE else 0x0F)
        if rl == 0:
            if q + 1 > len(src):
                raise Fail("truncated fgbg length")
            return src[q] + 1, q + 1
        return rl * 8, q
    if code in (REGULAR_BG_RUN, REGULAR_FG_RUN, REGULAR_COLOR_RUN, REGULAR_COLOR_IMAGE):
        rl = hdr & 0x1F
        if rl == 0:
            if q + 1 > len(src):
                raise Fail("truncated regular length")
            return src[q] + 32, q + 1
        return rl, q
    if code in (LITE_SET_FG_FG_RUN, LITE_DITHERED_RUN):
        rl = hdr & 0x0F
        if rl == 0:
            if q + 1 > len(src):
                raise Fail("truncated lite length")
            return src[q] + 16, q + 1
        return rl, q
    if code in (MEGA_MEGA_BG_RUN, MEGA_MEGA_FG_RUN, MEGA_MEGA_SET_FG_RUN,
                MEGA_MEGA_DITHERED_RUN, MEGA_MEGA_COLOR_RUN, MEGA_MEGA_FGBG_IMAGE,
                MEGA_MEGA_SET_FGBG_IMAGE, MEGA_MEGA_COLOR_IMAGE):
        if q + 2 > len(src):
            raise Fail("truncated mega length")
        return src[q] | (src[q + 1] << 8), q + 2
    if code in (SPECIAL_FGBG_1, SPECIAL_FGBG_2, SPECIAL_WHITE, SPECIAL_BLACK):
        return 0, q              # no run length: the code byte is the order
    raise Fail("bad code 0x%02x at %d" % (code, p))


def decode(data, w, h, pb=2, trace=False):
    """Returns (pixels in stream order, pixels written, order trace)."""
    src = data
    total = w * h
    row = w
    dst = [0] * total
    white = (1 << (8 * pb)) - 1
    fg = white
    ins = False
    first = True
    di = 0
    p = 0
    orders = []

    while p < len(src):
        if first and di >= row:
            first = False
            ins = False
        code = code_id(src[p])
        rl, p2 = run_length(code, src, p)

        if code in (REGULAR_BG_RUN, MEGA_MEGA_BG_RUN):
            p = p2
            if first:
                if ins:
                    dst[di] = 0
                    di += 1
                    rl -= 1
                if di + rl > total:
                    raise Fail("overrun in bg run")
                for _ in range(rl):
                    dst[di] = 0
                    di += 1
            else:
                if ins:
                    dst[di] = dst[di - row] ^ fg
                    di += 1
                    rl -= 1
                if di + rl > total:
                    raise Fail("overrun in bg run")
                for _ in range(rl):
                    dst[di] = dst[di - row]
                    di += 1
            ins = True
            orders.append((p2, NAMES[code], rl))
            continue

        ins = False

        if code in (REGULAR_FG_RUN, MEGA_MEGA_FG_RUN, LITE_SET_FG_FG_RUN, MEGA_MEGA_SET_FG_RUN):
            if code in (LITE_SET_FG_FG_RUN, MEGA_MEGA_SET_FG_RUN):
                fg, p2 = read_px(src, p2, pb)
            p = p2
            if di + rl > total:
                raise Fail("overrun in fg run")
            if first:
                for _ in range(rl):
                    dst[di] = fg
                    di += 1
            else:
                for _ in range(rl):
                    dst[di] = dst[di - row] ^ fg
                    di += 1
            orders.append((p, NAMES[code], rl))
        elif code in (LITE_DITHERED_RUN, MEGA_MEGA_DITHERED_RUN):
            a, p2 = read_px(src, p2, pb)
            b, p2 = read_px(src, p2, pb)
            p = p2
            if di + rl * 2 > total:
                raise Fail("overrun in dithered run")
            for _ in range(rl):
                dst[di] = a
                di += 1
                dst[di] = b
                di += 1
            orders.append((p, NAMES[code], rl * 2))
        elif code in (REGULAR_COLOR_RUN, MEGA_MEGA_COLOR_RUN):
            a, p2 = read_px(src, p2, pb)
            p = p2
            if di + rl > total:
                raise Fail("overrun in colour run")
            for _ in range(rl):
                dst[di] = a
                di += 1
            orders.append((p, NAMES[code], rl))
        elif code in (REGULAR_FGBG_IMAGE, MEGA_MEGA_FGBG_IMAGE,
                      LITE_SET_FG_FGBG_IMAGE, MEGA_MEGA_SET_FGBG_IMAGE):
            if code in (LITE_SET_FG_FGBG_IMAGE, MEGA_MEGA_SET_FGBG_IMAGE):
                fg, p2 = read_px(src, p2, pb)
            p = p2
            while rl > 8:
                if p >= len(src) or di + 8 > total:
                    raise Fail("overrun in fgbg image")
                mask = src[p]
                p += 1
                for i in range(8):
                    up = 0 if first else dst[di - row]
                    dst[di] = (up ^ fg) if (mask >> i) & 1 else up
                    di += 1
                rl -= 8
            if rl:
                if p >= len(src) or di + rl > total:
                    raise Fail("overrun in fgbg image")
                mask = src[p]
                p += 1
                for i in range(rl):
                    up = 0 if first else dst[di - row]
                    dst[di] = (up ^ fg) if (mask >> i) & 1 else up
                    di += 1
            orders.append((p, NAMES[code], "fgbg"))
        elif code in (REGULAR_COLOR_IMAGE, MEGA_MEGA_COLOR_IMAGE):
            p = p2
            if di + rl > total:
                raise Fail("overrun in colour image")
            for _ in range(rl):
                dst[di], p = read_px(src, p, pb)
                di += 1
            orders.append((p, NAMES[code], rl))
        elif code in (SPECIAL_FGBG_1, SPECIAL_FGBG_2):
            mask = 0x03 if code == SPECIAL_FGBG_1 else 0x05
            p = p2
            if di + 8 > total:
                raise Fail("overrun in special fgbg")
            for i in range(8):
                up = 0 if first else dst[di - row]
                dst[di] = (up ^ fg) if (mask >> i) & 1 else up
                di += 1
            orders.append((p, NAMES[code], 8))
        elif code == SPECIAL_WHITE:
            p = p2
            dst[di] = white
            di += 1
            orders.append((p, NAMES[code], 1))
        elif code == SPECIAL_BLACK:
            p = p2
            dst[di] = 0
            di += 1
            orders.append((p, NAMES[code], 1))
        else:
            raise Fail("unhandled code 0x%02x" % code)

    return dst, di, orders


if __name__ == "__main__":
    hexdata = sys.argv[1] if len(sys.argv) > 1 else \
        "001c840000000000000000f0bc0f840000000000000000"
    data = bytes.fromhex(hexdata)
    print("bytes=%d  %s" % (len(data), hexdata))
    dst, di, orders = decode(data, 64, 64)
    print("wrote %d of %d pixels (%d rows)" % (di, 64 * 64, di / 64.0))
    for o in orders:
        print("   after src@%2d %-24s %s" % (o[0], o[1], o[2]))
    for y in range(64):
        row = dst[y * 64:(y + 1) * 64]
        if len(set(row)) == 1:
            print("  row %2d: solid 0x%04x" % (y, row[0]))
        else:
            print("  row %2d: %s" % (y, "".join("#" if v else "." for v in row)))
