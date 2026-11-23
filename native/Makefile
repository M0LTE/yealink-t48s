# Programs for the T48S (ARMv6, Linux 2.6.27, EABI soft-float).
# Freestanding ones use raw syscalls; the rest link musl built by build-dropbear.sh.
# The Buildroot package (buildroot/package/t48s-tools) sets CROSS_CC and MKFONT.
T ?= $(HOME)/src-build/t48s
CROSS_CC ?= $(T)/bin/armcc
MKFONT ?= ../tools/mkfont.py
FREE = clang --target=armv6-linux-gnueabi -march=armv6 -mfloat-abi=soft -marm -Os \
       -ffreestanding -fno-builtin -nostdlib -fuse-ld=lld
MUSLCC = $(CROSS_CC) -Os -Wall

all: fbtest fbswap.so envexec fbinfo tap sertest usbinit t48ui membanner cpubench keytest touchtest waterfall

fbtest: fbtest.c
	$(FREE) -static -Wl,-e,_start -o $@ $<

# Loaded into Yealink's dskPhone with LD_PRELOAD; only errno comes from the host glibc
fbswap.so: fbswap.c
	$(FREE) -O2 -fPIC -shared -Wl,--hash-style=both -Wl,-soname,fbswap.so -o $@ $<

envexec fbinfo tap sertest usbinit keytest touchtest: %: %.c
	$(MUSLCC) -o $@ $<

gen/fonts.h: $(MKFONT)
	mkdir -p gen && python3 $(MKFONT) $@

gen/wf_fonts.h: $(MKFONT)
	mkdir -p gen && python3 $(MKFONT) $@ wf

t48ui: t48ui.c gen/fonts.h
	$(MUSLCC) -o $@ t48ui.c

clean:
	rm -f fbtest fbswap.so envexec fbinfo tap sertest usbinit t48ui membanner cpubench touchtest waterfall waterfall-host

membanner: membanner.c gen/fonts.h
	$(MUSLCC) -o $@ membanner.c

# CPU speed comparison between kernels (issue #9)
cpubench: cpubench.c
	$(MUSLCC) -O2 -o $@ $<

# Live receive waterfall of a pdn-soundmodem station (issue #13). waterfall-host
# is the same program for this PC: run it with --shot to render into a file.
WF_SRC = waterfall.c wf_fold.c ws_parse.c
WF_DEP = $(WF_SRC) wf_fold.h ws_parse.h gen/wf_fonts.h
waterfall: $(WF_DEP)
	$(MUSLCC) -O2 -s -o $@ $(WF_SRC) -lm

waterfall-host: $(WF_DEP)
	cc -O2 -Wall -Wextra -Wno-format-truncation -D_GNU_SOURCE -o $@ $(WF_SRC) -lm
