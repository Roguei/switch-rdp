#!/usr/bin/env python3
"""build_nro.py - turn a libnx-built Switch ELF into a loadable NRO.

Usage:  build_nro.py <input.elf> <output.nro>

Why this exists (2026-10-23 hardware bring-up):
  The previous PowerShell packer wrote the NroHeader's segment table with the
  sizes/offsets divided by 256, so the loader mapped text/ro/data at the wrong
  lengths.  Every devkitPro homebrew also needs the ELF to be *position
  independent* (-fPIE) and linked with -z text, otherwise const data holding
  pointers lands in the read-only segment and the loader's relocation pass
  faults on real hardware (emulators map everything writable, hiding it).

NRO layout (as consumed by Atmosphère's hbldr / hbmenu):

    0x00  u32  branch instruction ("b 0x80", comes from crt0)
    0x04  u32  mod_offset (offset of the MOD0 header, e.g. 0xF0)
    0x08  u8[8] "HOMEBREW"
    0x10  u32  magic "NRO0"
    0x14  u32  version (0)
    0x18  u32  size          total size of the NRO image (header + segments)
    0x1C  u32  flags (0)
    0x20  u32  text.offset,  0x24 u32 text.size
    0x28  u32  ro.offset,    0x2C u32 ro.size
    0x30  u32  data.offset,  0x34 u32 data.size
    0x38  u32  bss.size
    0x3C  u32  reserved
    0x40  ..   module id / build id / padding (zero)

  Segments are contiguous and start at offset 0 (the 0x80-byte header block is
  part of text).  Every offset/size is page aligned (0x1000).
"""

import os
import struct
import sys

PAGE = 0x1000
HDR_SIZE = 0x80

# NRO asset section (what elf2nro appends when it is given --nacp/--icon):
# it lives *after* the image, and every offset inside it is relative to the
# ASET header itself.  Ryujinx >= 1.1.1x calls Nro.OpenNroAssetSection()
# unconditionally, so a homebrew NRO without this section dies with a
# NullReferenceException inside LibHac (OpenNroAssetSection -> _assetHeader is
# null) instead of just showing an unknown title.  Hardware does not need it,
# but the emulator does, so it is always written.
ASET_HDR_SIZE = 0x38          # magic + version + 3 x (u64 offset, u64 size)
NACP_SIZE = 0x4000
NACP_NAME_LEN = 0x200
NACP_PUB_LEN = 0x100


def build_nacp(name, publisher):
    """0x4000-byte NACP: magic, version, 16 localized names, 16 publishers."""
    nacp = bytearray(NACP_SIZE)
    nacp[0:4] = b"NACP"
    struct.pack_into("<I", nacp, 4, 0)
    for lang in range(16):
        off = 8 + lang * NACP_NAME_LEN
        b = name.encode("utf-8")[:NACP_NAME_LEN - 1]
        nacp[off:off + len(b)] = b
        off = 8 + 16 * NACP_NAME_LEN + lang * NACP_PUB_LEN
        b = publisher.encode("utf-8")[:NACP_PUB_LEN - 1]
        nacp[off:off + len(b)] = b
    return bytes(nacp)


def find_icon():
    """Optional JPEG icon: project assets first, then devkitPro's default."""
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (os.path.join(here, "..", "assets", "icon.jpg"),            # repo layout
                 os.path.join(here, "..", "APP", "switch_rdp", "assets", "icon.jpg"),
                 "C:/devkitPro/libnx/default_icon.jpg",
                 "C:/devkitPro/nx/default_icon.jpg"):
        if os.path.isfile(cand):
            with open(cand, "rb") as f:
                return f.read(), cand
    return b"", None


def build_assets(name, publisher):
    icon, icon_path = find_icon()
    nacp = build_nacp(name, publisher)
    icon_off = ASET_HDR_SIZE
    # 8-byte align the NACP: hbmenu reads both through the offsets, but the
    # packers that produce the homebrew everyone else ships keep them aligned.
    nacp_off = (icon_off + len(icon) + 7) // 8 * 8
    pad = nacp_off - (icon_off + len(icon))
    out = bytearray()
    out += b"ASET"
    out += struct.pack("<I", 0)
    # libnx NroAssetSection is u64 offset/size, offsets relative to ASET itself
    out += struct.pack("<QQ", icon_off, len(icon))
    out += struct.pack("<QQ", nacp_off, len(nacp))
    out += struct.pack("<QQ", 0, 0)                    # romfs: none
    out += icon
    out += b"\0" * pad
    out += nacp
    return bytes(out), icon_path


def align_up(v, a=PAGE):
    return (v + a - 1) // a * a


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    if len(args) != 2:
        print("usage: build_nro.py <input.elf> <output.nro> "
              "[--no-assets] [--name=TITLE] [--publisher=NAME]")
        return 2
    src, dst = args
    no_assets = "--no-assets" in flags
    name = "RDP"
    publisher = ""
    for f in flags:
        if f.startswith("--name="):
            name = f.split("=", 1)[1]
        elif f.startswith("--publisher="):
            publisher = f.split("=", 1)[1]
    elf = open(src, "rb").read()

    if elf[:4] != b"\x7fELF" or elf[4] != 2 or elf[5] != 1:
        raise SystemExit("not a little-endian ELF64")

    phoff = struct.unpack_from("<Q", elf, 0x20)[0]
    phentsize = struct.unpack_from("<H", elf, 0x36)[0]
    phnum = struct.unpack_from("<H", elf, 0x38)[0]

    loads = []
    for i in range(phnum):
        o = phoff + i * phentsize
        p_type = struct.unpack_from("<I", elf, o)[0]
        if p_type != 1:                      # PT_LOAD
            continue
        p_off = struct.unpack_from("<Q", elf, o + 0x08)[0]
        p_vaddr = struct.unpack_from("<Q", elf, o + 0x10)[0]
        p_filesz = struct.unpack_from("<Q", elf, o + 0x20)[0]
        p_memsz = struct.unpack_from("<Q", elf, o + 0x28)[0]
        loads.append([p_vaddr, p_off, p_filesz, p_memsz])

    loads.sort()
    if not loads:
        raise SystemExit("no PT_LOAD segments")
    if len(loads) != 3:
        print("WARN: %d PT_LOAD segments (devkitPro layout has 3)" % len(loads))

    base = loads[0][0]
    if base != 0:
        raise SystemExit("image does not start at vaddr 0")

    img_size = align_up(max(v + f for v, _, f, _ in loads))
    mem_end = max(v + m for v, _, _, m in loads)
    bss_size = mem_end - img_size
    if bss_size < 0:
        bss_size = 0

    img = bytearray(img_size)
    for vaddr, off, filesz, _ in loads:
        img[vaddr:vaddr + filesz] = elf[off:off + filesz]

    # --- sanity: the header block must be the crt0 stub, and empty after 0x10
    branch = struct.unpack_from("<I", img, 0)[0]
    if branch != 0x14000020:
        raise SystemExit("unexpected entry stub 0x%08X" % branch)
    if bytes(img[8:16]) != b"HOMEBREW":
        print("WARN: module marker is %r" % bytes(img[8:16]))
    if any(img[0x40:0x80]):
        print("WARN: 0x40..0x80 is not zero (module id area)")

    # --- segment table from the vaddr gaps (NOT cumulative file sizes)
    segs = []
    for i, (vaddr, off, filesz, _) in enumerate(loads):
        nxt = loads[i + 1][0] if i + 1 < len(loads) else img_size
        size = nxt - vaddr
        if size % PAGE:
            raise SystemExit("segment %d size 0x%X is not page aligned" % (i, size))
        if size < filesz:
            raise SystemExit("segment %d size 0x%X < filesz 0x%X" % (i, size, filesz))
        segs.append((vaddr, size))
    if segs[0][0] != 0:
        raise SystemExit("text must start at offset 0")
    if sum(s for _, s in segs) != img_size:
        raise SystemExit("segment table does not add up to the image size")

    text, ro, data = segs

    struct.pack_into("<I", img, 0x10, 0x304F524E)      # "NRO0"
    struct.pack_into("<I", img, 0x14, 0)
    struct.pack_into("<I", img, 0x18, img_size)
    struct.pack_into("<I", img, 0x1C, 0)
    struct.pack_into("<II", img, 0x20, text[0], text[1])
    struct.pack_into("<II", img, 0x28, ro[0], ro[1])
    struct.pack_into("<II", img, 0x30, data[0], data[1])
    struct.pack_into("<I", img, 0x38, bss_size)
    struct.pack_into("<I", img, 0x3C, 0)

    # --- trailing asset section (NACP + optional icon) for emulator loaders
    # Ryujinx needs it, hbmenu/Atmosphere do not, and a plain image is the exact
    # shape that is known to boot on hardware, so it can be switched off.
    image_size = len(img)
    if no_assets:
        assets, icon_path = b"", None
    else:
        assets, icon_path = build_assets(name, publisher)
    img += assets

    open(dst, "wb").write(img)

    print("ELF   : %s" % src)
    print("LOAD  : " + "  ".join("v=0x%X off=0x%X fsz=0x%X msz=0x%X" % tuple(l)
                                 for l in loads))
    print("image : 0x%X  bss=0x%X" % (image_size, bss_size))
    print("text  : off=0x%X size=0x%X" % text)
    print("ro    : off=0x%X size=0x%X" % ro)
    print("data  : off=0x%X size=0x%X" % data)
    print("assets: ASET at 0x%X (+%d bytes, icon=%s)"
          % (image_size, len(assets), icon_path or "none"))
    print("NRO   : %s -> %d bytes" % (dst, len(img)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
