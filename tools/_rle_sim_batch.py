"""Batch driver for rle_sim.py: same interface and hash as tools/_rle_test.exe."""
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])

from rle_sim import decode  # noqa: E402


def to_rgb(px, w, h, pb, bpp):
    out = bytearray(w * h * 3)
    for y in range(h):
        base = (h - 1 - y) * w * 3
        srow = y * w
        for x in range(w):
            v = px[srow + x]
            if pb == 2:
                if bpp == 15:
                    r = ((v >> 10) & 0x1F) << 3
                    g = ((v >> 5) & 0x1F) << 3
                    b = (v & 0x1F) << 3
                else:
                    r = ((v >> 11) & 0x1F) << 3
                    g = ((v >> 5) & 0x3F) << 2
                    b = (v & 0x1F) << 3
            elif pb == 1:
                r = g = b = v
            else:
                b = v & 0xFF
                g = (v >> 8) & 0xFF
                r = (v >> 16) & 0xFF
            o = base + x * 3
            out[o] = r
            out[o + 1] = g
            out[o + 2] = b
    return out


def fnv(data):
    h = 2166136261
    for byte in data:
        h ^= byte
        h = (h * 16777619) & 0xFFFFFFFF
    return h


for line in sys.stdin:
    parts = line.split()
    if len(parts) < 4:
        continue
    w, h, bpp = int(parts[0]), int(parts[1]), int(parts[2])
    try:
        data = bytes.fromhex(parts[3])
        pb = 2 if bpp in (15, 16) else bpp // 8
        px, di, _ = decode(data, w, h, pb)
        if di > w * h:
            raise ValueError("overrun")
        rgb = to_rgb(px, w, h, pb, bpp)
        print("rc=0 hash=%08x" % fnv(rgb))
    except Exception:
        print("rc=-1 hash=811c9dc5")
