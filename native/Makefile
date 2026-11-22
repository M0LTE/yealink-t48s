# Programs for the T48S (ARMv6, Linux 2.6.27, EABI soft-float).
# Freestanding ones use raw syscalls; the rest link musl built by build-dropbear.sh.
T ?= $(HOME)/src-build/t48s
FREE = clang --target=armv6-linux-gnueabi -march=armv6 -mfloat-abi=soft -marm -Os \
       -ffreestanding -fno-builtin -nostdlib -fuse-ld=lld
MUSLCC = $(T)/bin/armcc -Os -Wall

all: fbtest fbswap.so envexec fbinfo tap sertest usbinit t48ui membanner

fbtest: fbtest.c
	$(FREE) -static -Wl,-e,_start -o $@ $<

# Loaded into Yealink's dskPhone with LD_PRELOAD; only errno comes from the host glibc
fbswap.so: fbswap.c
	$(FREE) -O2 -fPIC -shared -Wl,--hash-style=both -Wl,-soname,fbswap.so -o $@ $<

envexec fbinfo tap sertest usbinit: %: %.c
	$(MUSLCC) -o $@ $<

gen/fonts.h: ../tools/mkfont.py
	python3 ../tools/mkfont.py $@

t48ui: t48ui.c gen/fonts.h
	$(MUSLCC) -o $@ t48ui.c

clean:
	rm -f fbtest fbswap.so envexec fbinfo tap sertest usbinit t48ui membanner

membanner: membanner.c gen/fonts.h
	$(MUSLCC) -o $@ membanner.c
