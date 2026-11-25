# SPDX-License-Identifier: AGPL-3.0-or-later
# t48-audiod, the audio service (issue #24, docs/audiod.md).
#   make                 this PC: build/t48-audiod, build/t48-audioctl
#   make check           unit tests and the socket test (no sound: file and null backends only)
#   make bench           cost of one 20 ms frame on this machine
#   make CC=$$HOME/src-build/t48s/bin/armcc O=build-arm    the phone: static ARM
O ?= build
# speexdsp's echo canceller for "echo aec" (third_party/speexdsp/README.t48):
# fixed point, its own integer FFT, built as its authors wrote it (no -Wall)
SPX = third_party/speexdsp
SPX_OBJS = $(addprefix $(O)/spx/, mdf.o kiss_fft.o kiss_fftr.o fftwrap.o)
SPX_FLAGS = -O2 -fno-tree-vectorize -ffunction-sections -w -I$(SPX) -DFIXED_POINT -DUSE_KISS_FFT -DEXPORT=
CC ?= cc
# the call stream's protocol header, shared with baresip's module
T48AUDIO ?= ../sip/modules/t48audio
# the app protocol's header, for the ring an app's sound comes in
LIBT48_INCLUDE ?= ../libt48/include
CFLAGS ?= -O2
CFLAGS += -Wall -Wextra -std=gnu99 -D_GNU_SOURCE -I. -I$(T48AUDIO) -I$(LIBT48_INCLUDE) -I$(SPX)
# speexdsp has two unused floating-point helpers: dropped at link time
LDFLAGS += -static -Wl,--gc-sections -lm

CORE = dsp.c tone.c echo.c aec.c pace.c engine.c routes.c msg.c appsnd.c $(SPX_OBJS)
SRCS = main.c hw_null.c hw_alsa.c $(CORE)
HDRS = $(wildcard *.h include/*.h) $(T48AUDIO)/t48audio_proto.h $(LIBT48_INCLUDE)/t48_proto.h
TESTS = test_dsp test_tone test_pace test_echo test_aec test_engine test_msg test_appsnd

all: $(O)/t48-audiod $(O)/t48-audioctl

$(O)/spx/%.o: $(SPX)/%.c $(wildcard $(SPX)/*.h $(SPX)/speex/*.h)
	@mkdir -p $(O)/spx
	$(CC) $(SPX_FLAGS) -c -o $@ $<

$(O)/t48-audiod: $(SRCS) $(HDRS)
	@mkdir -p $(O)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

$(O)/t48-audioctl: audioctl.c msg.c $(HDRS)
	@mkdir -p $(O)
	$(CC) $(CFLAGS) -o $@ audioctl.c msg.c $(LDFLAGS)

$(O)/%: tests/%.c $(CORE) hw_null.c $(HDRS) tests/t.h
	@mkdir -p $(O)
	$(CC) $(CFLAGS) -o $@ $< $(CORE) hw_null.c -lm

check: all $(TESTS:%=$(O)/%)
	@for t in $(TESTS); do $(O)/$$t || exit 1; done
	python3 tests/socket_test.py $(O)

# No auto-vectorising: the phone's ARM11 has no NEON, so SSE would flatter this PC
$(O)/bench: tests/bench.c $(CORE) $(HDRS) tests/t.h
	@mkdir -p $(O)
	$(CC) $(CFLAGS) -fno-tree-vectorize -o $@ tests/bench.c $(CORE) -lm $(LDFLAGS)

bench: $(O)/bench
	$(O)/bench

# A stand-in for baresip on the call stream, for measurements on the phone
$(O)/calltap: tests/calltap.c $(T48AUDIO)/t48audio_proto.h
	@mkdir -p $(O)
	$(CC) $(CFLAGS) -o $@ tests/calltap.c -lm $(LDFLAGS)

clean:
	rm -rf build build-arm

.PHONY: all check bench clean
