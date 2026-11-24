# Programs for the T48S (ARMv6, Linux 2.6.27, EABI soft-float).
# Freestanding ones use raw syscalls; the rest link musl built by build-dropbear.sh.
# The Buildroot package (buildroot/package/t48s-tools) sets CROSS_CC.
# gen/fonts.h is committed; "make fonts" regenerates it
# (needs python3-pil and the DejaVu fonts).
T ?= $(HOME)/src-build/t48s
CROSS_CC ?= $(T)/bin/armcc
MKFONT ?= ../tools/mkfont.py
FREE = clang --target=armv6-linux-gnueabi -march=armv6 -mfloat-abi=soft -marm -Os \
       -ffreestanding -fno-builtin -nostdlib -fuse-ld=lld
MUSLCC = $(CROSS_CC) -Os -Wall

all: fbtest fbswap.so envexec fbinfo tap sertest usbinit membanner cpubench keytest touchtest apmcap t48rec t48play t48mix nandread nandrestore t48-watchdog t48-keys t48-meter

fbtest: fbtest.c
	$(FREE) -static -Wl,-e,_start -o $@ $<

# Loaded into Yealink's dskPhone with LD_PRELOAD; only errno comes from the host glibc
fbswap.so: fbswap.c
	$(FREE) -O2 -fPIC -shared -Wl,--hash-style=both -Wl,-soname,fbswap.so -o $@ $<

envexec fbinfo tap sertest usbinit keytest touchtest: %: %.c
	$(MUSLCC) -o $@ $<

# Read-only NAND dump, runs under Yealink's kernel and ours (issue #5)
nandread: nandread.c
	$(MUSLCC) -static -o $@ $<

# Puts a nandread -o backup back (issue #30); -n runs on any PC
nandrestore: nandrestore.c
	$(MUSLCC) -static -o $@ $<

# Feeds the hardware watchdog (issue #17)
t48-watchdog: t48-watchdog.c
	$(MUSLCC) -static -o $@ $<

# Progress for t48-install: a pipe that counts (docs/installer.md)
t48-meter: t48-meter.c
	$(MUSLCC) -static -o $@ $<

# The keypad for the first stage's rescue menu (issue #32)
t48-keys: t48-keys.c
	$(MUSLCC) -static -o $@ $<

fonts:
	mkdir -p gen && python3 $(MKFONT) gen/fonts.h

.PHONY: all clean fonts

# Microphone capture straight from the APM block, no DSP (issue #6, docs/audio.md)
apmcap: apmcap.c
	$(MUSLCC) -static -o $@ $< -lm

# ALSA capture, playback and mixer test tools for kernel/drivers/t48s_audio.c (issue #24)
t48rec t48play: %: %.c
	$(MUSLCC) -static -o $@ $< -lm

t48mix: t48mix.c
	$(MUSLCC) -static -o $@ $<

clean:
	rm -f fbtest fbswap.so envexec fbinfo tap sertest usbinit membanner cpubench keytest touchtest apmcap t48rec t48play t48mix

membanner: membanner.c gen/fonts.h
	$(MUSLCC) -o $@ membanner.c

# CPU speed comparison between kernels (issue #9)
cpubench: cpubench.c
	$(MUSLCC) -O2 -o $@ $<

# The waterfall and the GB7RDG links screen (t48ui) are apps now, in
# M0LTE/yealink-t48s-apps (apps/waterfall, apps/links).
