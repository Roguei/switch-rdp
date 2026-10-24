#!/usr/bin/env python3
"""build_mbedtls.py - rebuild the bundled mbedTLS 2.28.8 for the Switch.

Why this exists:
    The first build of third_party/mbedtls was compiled WITHOUT -fPIC.  mbedTLS
    is full of const tables holding function pointers (ciphersuite lists, md
    info, curve tables...).  Built without -fPIC those tables land in .rodata,
    so the final ELF carries ~770 dynamic relocations against the read-only
    segment.  Ryujinx maps every segment writable and hides it, but on real
    hardware the loader maps the ro segment read-only and __nx_dynamic() dies
    while applying relocations (fatal 2168-0002, PC inside _dynProcessRela).
    libnx's switch.specs passes -z text, so a correct build fails loudly at
    link time if this ever regresses.

Usage:
    python tools/build_mbedtls.py            # full rebuild
    python tools/build_mbedtls.py --clean    # wipe obj dir first

Output:
    third_party/mbedtls-build/obj_pic/*.o
    third_party/mbedtls-install/lib/libmbedtls.a   (copied to the 3 names the
                                                    Makefile links against)
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
TP = os.path.join(PROJ, "third_party")
SRC = os.path.join(TP, "mbedtls-mbedtls-2.28.8")
LIB = os.path.join(SRC, "library")
OBJ = os.path.join(TP, "mbedtls-build", "obj_pic")
INSTALL_LIB = os.path.join(TP, "mbedtls-install", "lib")

DEVKITPRO = r"C:\devkitPro"
BIN = os.path.join(DEVKITPRO, "devkitA64", "bin")
CC = os.path.join(BIN, "aarch64-none-elf-gcc.exe")
AR = os.path.join(BIN, "aarch64-none-elf-ar.exe")

CFLAGS = [
    "-mcpu=cortex-a53+fp+simd", "-mtune=cortex-a53",
    "-O2", "-g", "-fPIC", "-fno-strict-aliasing",
    # strip the build machine path out of __FILE__ (mbedTLS prints it in its
    # error log); otherwise the released NRO carries the developer's home path.
    "-ffile-prefix-map=" + SRC + "=mbedtls",
    "-mstrict-align", "-D_DEFAULT_SOURCE",
    "-I" + os.path.join(SRC, "include"),
    # installed libnx headers (C:/devkitPro/libnx/{include,lib}); the old
    # "libnx/nx/..." layout only exists when libnx is used straight from its
    # source tree.
    "-I" + os.path.join(DEVKITPRO, "libnx", "include"),
]


def main():
    if "--clean" in sys.argv and os.path.isdir(OBJ):
        shutil.rmtree(OBJ)
    os.makedirs(OBJ, exist_ok=True)
    os.makedirs(INSTALL_LIB, exist_ok=True)

    sources = [os.path.join(LIB, f) for f in sorted(os.listdir(LIB))
               if f.endswith(".c")]
    # Horizon port: supplies mbedtls_hardware_poll (MBEDTLS_ENTROPY_HARDWARE_ALT)
    sources.append(os.path.join(TP, "mbedtls-port.c"))
    if not sources:
        raise SystemExit("no sources in " + LIB)

    objs = []
    failed = []
    for i, path in enumerate(sources, 1):
        name = os.path.basename(path)
        obj = os.path.join(OBJ, name[:-2] + ".o")
        objs.append(obj)
        if os.path.exists(obj) and os.path.getmtime(obj) > os.path.getmtime(path):
            continue
        cmd = [CC] + CFLAGS + ["-c", path, "-o", obj]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            failed.append((name, r.stderr.strip().splitlines()[:3]))
        if i % 20 == 0:
            print("  %d/%d" % (i, len(sources)))

    if failed:
        for name, err in failed:
            print("FAILED %s" % name)
            for line in err:
                print("   " + line)
        raise SystemExit(1)

    archive = os.path.join(INSTALL_LIB, "libmbedtls.a")
    if os.path.exists(archive):
        os.remove(archive)
    r = subprocess.run([AR, "rcs", archive] + objs, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("ar failed: " + r.stderr)

    # The Makefile links -lmbedtls -lmbedcrypto -lmbedx509; ship one archive
    # under all three names (they were identical before, keep it that way).
    for name in ("libmbedcrypto.a", "libmbedx509.a"):
        shutil.copyfile(archive, os.path.join(INSTALL_LIB, name))

    print("objects : %d" % len(objs))
    print("archive : %s (%d bytes)" % (archive, os.path.getsize(archive)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
