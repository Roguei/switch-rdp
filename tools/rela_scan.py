import struct
import sys

path = sys.argv[1] if len(sys.argv) > 1 else None
if not path:
    print("usage: rela_scan.py <elf>")
    sys.exit(2)

data = open(path, "rb").read()

# ELF64 little endian
assert data[:4] == b"\x7fELF"
e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
e_shentsize = struct.unpack_from("<H", data, 0x3A)[0]
e_shnum = struct.unpack_from("<H", data, 0x3C)[0]
e_shstrndx = struct.unpack_from("<H", data, 0x3E)[0]

shstr_off = struct.unpack_from("<Q", data, e_shoff + e_shstrndx * e_shentsize + 0x18)[0]

sections = {}
for i in range(e_shnum):
    o = e_shoff + i * e_shentsize
    name_off = struct.unpack_from("<I", data, o)[0]
    end = data.index(b"\0", shstr_off + name_off)
    name = data[shstr_off + name_off:end].decode()
    addr = struct.unpack_from("<Q", data, o + 0x10)[0]
    off = struct.unpack_from("<Q", data, o + 0x18)[0]
    size = struct.unpack_from("<Q", data, o + 0x20)[0]
    sections[name] = (addr, off, size)

# segment ranges straight from the program headers (do not hardcode: the
# layout changes whenever the code grows)
phoff = struct.unpack_from("<Q", data, 0x20)[0]
phentsize = struct.unpack_from("<H", data, 0x36)[0]
phnum = struct.unpack_from("<H", data, 0x38)[0]
seg = []
_names = ["text", "ro", "data", "seg3", "seg4"]
for i in range(phnum):
    o = phoff + i * phentsize
    if struct.unpack_from("<I", data, o)[0] != 1:
        continue
    vaddr = struct.unpack_from("<Q", data, o + 0x10)[0]
    memsz = struct.unpack_from("<Q", data, o + 0x28)[0]
    seg.append((_names[len(seg)], vaddr, (vaddr + memsz + 0xFFF) & ~0xFFF))


def which(va):
    for n, lo, hi in seg:
        if lo <= va < hi:
            return n
    return "outside"


if ".rela.dyn" not in sections:
    print("no .rela.dyn - image is RELR-packed (-z pack-relative-relocs);")
    print("libnx's __nx_dynamic must then handle DT_RELR.  Add")
    print("  -Wl,-z,nopack-relative-relocs")
    print("to LDFLAGS to get the proven .rela path back.")
    raise SystemExit(0)
rela_addr, rela_off, rela_size = sections[".rela.dyn"]
print(".rela.dyn va=0x%X off=0x%X size=0x%X entries=%d" %
      (rela_addr, rela_off, rela_size, rela_size // 24))

from collections import Counter
counts = Counter()
bad = []
for i in range(rela_size // 24):
    o = rela_off + i * 24
    r_offset, r_info, r_addend = struct.unpack_from("<QQq", data, o)
    rtype = r_info & 0xFFFFFFFF
    where = which(r_offset)
    counts[(where, rtype)] += 1
    if where == "ro":
        bad.append((r_offset, rtype, r_addend, i))

print("by segment/type:", dict(counts))
print("relocations into the read-only segment:", len(bad))
for r_offset, rtype, r_addend, i in bad[:20]:
    print("  idx=%d r_offset=0x%X type=%d addend=0x%X" % (i, r_offset, rtype, r_addend))

# what lives at 0xCD208
va, off, size = sections[".rodata"]
print(".rodata va=0x%X size=0x%X" % (va, size))
tgt = 0xCD208
if va <= tgt < va + size:
    fo = off + (tgt - va)
    print("bytes at 0x%X: %s" % (tgt, data[fo:fo + 16].hex(" ")))
