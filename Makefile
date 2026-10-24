# switch_rdp Makefile
#
# Self-contained RDP client: libnx + mbedTLS only.  No FreeRDP / OpenSSL.
#
# Two things here are load-bearing, do not "simplify" them:
#   * -fPIE on the compile line and -pie/-z text from libnx's own switch.specs.
#     Without them const data that holds pointers ends up in the read-only
#     segment and the loader's relocation pass faults on real hardware (an
#     emulator maps everything writable and hides the bug).  -z text makes the
#     linker refuse to produce such an image at all.
#   * Short (8.3) paths in MBEDTLS / TOOLCHAIN when the checkout sits under a
#     non-ASCII directory: make recipes can run through cmd.exe, which does not
#     understand the UTF-8 name.  Both are overridable:
#       make MBEDTLS=/path/to/mbedtls-install PYTHON=python
#
# Requirements: devkitPro (devkitA64 + libnx), mbedTLS 2.28.x built for
# aarch64-none-elf (tools/build_mbedtls.py), Python 3 with Pillow (icon/font
# generators only).

CC = C:/devkitPro/devkitA64/bin/aarch64-none-elf-gcc.exe
LD = C:/devkitPro/devkitA64/bin/aarch64-none-elf-gcc.exe

# libnx's switch.specs uses %:getenv(DEVKITPRO ...), so export it.
export DEVKITPRO := C:/devkitPro
LIBNX   = $(DEVKITPRO)/libnx

# Defaults point at the layout this was developed in; override on the command
# line for a fresh checkout.  TOOLCHAIN is only used for build_nro.py, which
# lives in tools/ here.
MBEDTLS   ?= C:/Users/ADMINI~1/DOCUME~1/SWITCH~1/APP/SWITCH~1/third_party/mbedtls-install
TOOLCHAIN ?= tools
PYTHON    ?= C:/Users/Administrator/AppData/Local/Programs/Python/Python314/python.exe

INCLUDES = -I$(LIBNX)/include \
           -I$(MBEDTLS)/include \
           -Isrc

NORMAL_LIBS = -L$(LIBNX)/lib -L$(MBEDTLS)/lib -lnx -lm -lmbedtls -lmbedcrypto -lmbedx509

CFLAGS = -mcpu=cortex-a53+fp+simd -mtune=cortex-a53 -O3 -g -Wall \
         -fPIE -fno-strict-aliasing -D_GNU_SOURCE -D__SWITCH__

# switch.specs supplies -pie -z text -z now -z pack-relative-relocs
# --spare-dynamic-tags=0 --nx-module-name plus the linker script.
# -z nopack-relative-relocs: the upgraded binutils now honours the specs'
# "-z pack-relative-relocs" and emits .relr.dyn instead of .rela.dyn.  libnx's
# loader handles RELR, but the .rela path is the one that has been proven on
# hardware, so keep it until RELR has been tested there.
LDFLAGS = -specs=$(LIBNX)/switch.specs -Wl,--no-gc-sections -Wl,-z,nopack-relative-relocs

SRCS = src/main.c \
       src/ui_draw.c \
       src/ui_font.c \
       src/rdp_crypto.c \
       src/rdp_tls.c \
       src/rdp_credssp.c \
       src/rdp_mcs.c \
       src/rdp_planar.c \
       src/rdp_update.c \
       src/rdp_input.c \
       src/rdp_session.c

OBJS = $(SRCS:.c=.o)
ELF = build/switch_rdp.elf
TARGET = build/switch_rdp.nro
# Title/publisher written into the NACP (what hbmenu shows next to the icon).
NACP_NAME = RDP
NACP_PUB  = roguei
NRO_ARGS  = --name=$(NACP_NAME) --publisher=$(NACP_PUB)
# Both variants carry the same image plus the trailing ASET asset section
# (icon + NACP).  hbmenu shows the icon and title from it, and Ryujinx >= 1.10
# refuses to load an NRO without it (LibHac NullReferenceException).
HW_NRO = build/switch_rdp_hw.nro
# Bare image, no ASET: only for troubleshooting a loader that chokes on the
# trailing section - it shows up as a generic entry in hbmenu.
HW_BARE = build/switch_rdp_hw_bare.nro

all: $(TARGET)

hw: $(HW_NRO)

hw-bare: $(HW_BARE)

$(ELF): $(OBJS)
	@$(PYTHON) -c "import os; os.makedirs('build', exist_ok=True)"
	$(LD) $(CFLAGS) $(LDFLAGS) $(OBJS) $(NORMAL_LIBS) -o $@

$(TARGET): $(ELF)
	$(PYTHON) $(TOOLCHAIN)/build_nro.py $(ELF) $@ $(NRO_ARGS)

$(HW_NRO): $(ELF)
	$(PYTHON) $(TOOLCHAIN)/build_nro.py $(ELF) $@ $(NRO_ARGS)

$(HW_BARE): $(ELF)
	$(PYTHON) $(TOOLCHAIN)/build_nro.py $(ELF) $@ --no-assets

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Deliberately explicit: build/ also holds files that are not ours
# (a large .nsz was dropped in there once); never glob-delete it.
#
# The recipes avoid both cmd.exe and sh builtins: which shell make picks
# depends on whether sh.exe happens to be on PATH, and "if not exist ..."
# (cmd) or "mkdir -p" (sh) then fails with "syntax error: unexpected end of
# file" for one of them.  Python does the same job in either shell.
clean:
	@-$(PYTHON) -c "import glob, os; [os.remove(f) for f in glob.glob('src/*.o') + ['build/switch_rdp.elf', 'build/switch_rdp.nro', 'build/switch_rdp_hw.nro', 'build/switch_rdp_hw_bare.nro'] if os.path.exists(f)]"

.PHONY: all hw hw-bare clean
