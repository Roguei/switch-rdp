"""check_nro.py - sanity check a built .nro (header, segments, NACP, icon).

    python tools/check_nro.py build/switch_rdp_hw.nro

Verifies the things that actually break a homebrew on hardware:
  * NroHeader magic/size and that the segment table adds up,
  * the trailing ASET asset section (offsets inside the file, JPEG icon,
    0x4000-byte NACP with the expected magic and title),
  * that the file is 4-byte aligned in size and does not claim a size larger
    than the file (both make hbmenu/hbldr skip the file).
"""

import os
import struct
import sys

NRO_MAGIC = 0x304F524E          # "NRO0"
ASET_MAGIC = b"ASET"


def check(path):
    data = open(path, "rb").read()
    ok = True
    problems = []

    if len(data) < 0x80:
        return ["file shorter than the NRO header"]

    magic = struct.unpack_from("<I", data, 0x10)[0]
    version = struct.unpack_from("<I", data, 0x14)[0]
    size = struct.unpack_from("<I", data, 0x18)[0]
    segs = [struct.unpack_from("<II", data, 0x20 + 8 * i) for i in range(3)]
    bss = struct.unpack_from("<I", data, 0x38)[0]

    if magic != NRO_MAGIC:
        problems.append("bad magic 0x%08X (want 0x%08X)" % (magic, NRO_MAGIC))
    if size > len(data):
        problems.append("header size 0x%X > file 0x%X" % (size, len(data)))
    if size % 4:
        problems.append("header size 0x%X is not 4-byte aligned" % size)
    total = sum(s for _, s in segs)
    if total != size:
        problems.append("segments add up to 0x%X, header says 0x%X" % (total, size))
    for i, (off, sz) in enumerate(segs):
        if off + sz > size:
            problems.append("segment %d (0x%X+0x%X) runs past the image" % (i, off, sz))

    print("%s: %d bytes, image 0x%X, bss 0x%X, version %d" % (path, len(data), size, bss, version))
    print("  segments: " + ", ".join("0x%X+0x%X" % s for s in segs))

    if size >= len(data):
        print("  assets  : none (bare image)")
        return problems

    tag = data[size:size + 4]
    if tag != ASET_MAGIC:
        problems.append("no ASET section at 0x%X (found %r)" % (size, tag))
        return problems

    ver = struct.unpack_from("<I", data, size + 4)[0]
    icon_off, icon_size = struct.unpack_from("<QQ", data, size + 8)
    nacp_off, nacp_size = struct.unpack_from("<QQ", data, size + 24)
    romfs_off, romfs_size = struct.unpack_from("<QQ", data, size + 40)
    print("  assets  : ASET v%d icon=%d@0x%X nacp=%d@0x%X romfs=%d@0x%X"
          % (ver, icon_size, icon_off, nacp_size, nacp_off, romfs_size, romfs_off))

    icon = data[size + icon_off:size + icon_off + icon_size]
    if not icon.startswith(b"\xff\xd8"):
        problems.append("icon is not a JPEG")
    else:
        w = h = 0
        i = 2
        while i < len(icon) - 9:
            if icon[i] != 0xFF:
                break
            marker = icon[i + 1]
            if marker in (0xC0, 0xC1, 0xC2):
                h, w = struct.unpack(">HH", icon[i + 5:i + 9])
                break
            i += 2 + struct.unpack(">H", icon[i + 2:i + 4])[0]
        print("  icon    : JPEG %dx%d, %d bytes" % (w, h, len(icon)))
        if (w, h) != (256, 256):
            problems.append("icon is %dx%d, the asset section wants 256x256" % (w, h))

    nacp = data[size + nacp_off:size + nacp_off + nacp_size]
    if nacp[:4] != b"NACP":
        problems.append("NACP magic is %r" % nacp[:4])
    else:
        name = nacp[8:8 + 0x200].split(b"\0")[0].decode("utf-8", "replace")
        pub = nacp[8 + 16 * 0x200:8 + 16 * 0x200 + 0x100].split(b"\0")[0].decode("utf-8", "replace")
        print("  nacp    : %d bytes, name=%r publisher=%r" % (nacp_size, name, pub))
        if not name:
            problems.append("NACP title is empty")
        if nacp_size != 0x4000:
            problems.append("NACP is %d bytes, expected 0x4000" % nacp_size)

    return problems


def main():
    if len(sys.argv) < 2:
        print("usage: check_nro.py <file.nro> [more.nro ...]")
        return 2
    bad = 0
    for path in sys.argv[1:]:
        if not os.path.isfile(path):
            print("%s: missing" % path)
            bad += 1
            continue
        problems = check(path)
        if problems:
            bad += 1
            for p in problems:
                print("  PROBLEM: %s" % p)
        else:
            print("  ok")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
