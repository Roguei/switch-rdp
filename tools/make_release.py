"""make_release.py - assemble the release artifacts and the install zip.

    python tools/make_release.py b43

Produces, in dist/:
    RDP_<ver>.nro        image + icon/NACP (what users copy to SD:/switch/)
    RDP_<ver>_bare.nro   same image without the ASET asset section
    RDP_<ver>.zip        SD-card layout: switch/RDP.nro + cfg example + README

The zip is meant to be extracted at the root of the SD card.
"""

import os
import shutil
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
DIST = os.path.join(ROOT, "dist")

HW = os.path.join(BUILD, "switch_rdp_hw.nro")
BARE = os.path.join(BUILD, "switch_rdp_hw_bare.nro")
CFG = os.path.join(ROOT, "switch_rdp.cfg.example")
README = os.path.join(ROOT, "README.md")
LICENSE = os.path.join(ROOT, "LICENSE")


def main():
    ver = sys.argv[1] if len(sys.argv) > 1 else "dev"
    for path in (HW, CFG, README, LICENSE):
        if not os.path.isfile(path):
            raise SystemExit("missing %s (run make hw first)" % path)
    os.makedirs(DIST, exist_ok=True)

    nro = os.path.join(DIST, "RDP_%s.nro" % ver)
    shutil.copyfile(HW, nro)
    out = [nro]

    if os.path.isfile(BARE):
        bare = os.path.join(DIST, "RDP_%s_bare.nro" % ver)
        shutil.copyfile(BARE, bare)
        out.append(bare)

    zpath = os.path.join(DIST, "RDP_%s.zip" % ver)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(nro, "switch/RDP.nro")
        z.write(CFG, "switch_rdp.cfg.example")
        z.write(README, "README.md")
        z.write(LICENSE, "LICENSE")
    out.append(zpath)

    for p in out:
        print("%-28s %9d bytes" % (os.path.basename(p), os.path.getsize(p)))


if __name__ == "__main__":
    main()
