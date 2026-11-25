# SPDX-License-Identifier: AGPL-3.0-or-later
# libt48: the app library. Plain make.
#   make                                   for this PC, into build/
#   make check                             the sound tests
#   make CC=~/src-build/t48s/bin/armcc O=build-arm     for the phone (static, musl)
CC ?= cc
AR ?= ar
O ?= build
CFLAGS ?= -O2
CFLAGS += -std=gnu99 -Wall -Wextra -Wno-format-truncation -D_GNU_SOURCE -ffunction-sections -fdata-sections -Iinclude

OBJ = $(O)/draw.o $(O)/image.o $(O)/json.o $(O)/app.o $(O)/ws.o $(O)/kbd.o $(O)/theme.o $(O)/ui.o $(O)/sound.o \
	$(O)/fonts_gen.o $(O)/fonts_plex.o $(O)/fonts_figtree.o

all: $(O)/libt48.a $(O)/examples/clock/clock $(O)/examples/fliptest/fliptest $(O)/examples/tones/tones

$(O)/libt48.a: $(OBJ)
	rm -f $@ && $(AR) rcs $@ $(OBJ)

$(O)/%.o: src/%.c include/t48.h include/t48_proto.h
	@mkdir -p $(O)
	$(CC) $(CFLAGS) -c -o $@ $<

# the example app, laid out as an installed app: a folder with a manifest
$(O)/examples/clock/clock: examples/clock/clock.c $(O)/libt48.a examples/clock/manifest.json examples/clock/icon.ppm
	@mkdir -p $(O)/examples/clock
	$(CC) $(CFLAGS) -Wl,--gc-sections -o $@ $< $(O)/libt48.a
	cp examples/clock/manifest.json examples/clock/icon.ppm $(O)/examples/clock/

# the flipping test, and the example of a program that flips (API level 3)
$(O)/examples/fliptest/fliptest: examples/fliptest/fliptest.c $(O)/libt48.a examples/fliptest/manifest.json
	@mkdir -p $(O)/examples/fliptest
	$(CC) $(CFLAGS) -Wl,--gc-sections -o $@ $< $(O)/libt48.a
	cp examples/fliptest/manifest.json $(O)/examples/fliptest/

# sound from an app (API level 4): four notes
$(O)/examples/tones/tones: examples/tones/tones.c $(O)/libt48.a examples/tones/manifest.json
	@mkdir -p $(O)/examples/tones
	$(CC) $(CFLAGS) -Wl,--gc-sections -o $@ $< $(O)/libt48.a -lm
	cp examples/tones/manifest.json $(O)/examples/tones/

# the resampler and the sound queue with nobody taking (no shell needed)
$(O)/test_sound: tests/test_sound.c $(O)/libt48.a
	$(CC) $(CFLAGS) -o $@ $< $(O)/libt48.a -lm

check: $(O)/test_sound
	$(O)/test_sound

# regenerate the committed font tables from fonts/ (needs python3-pil)
fonts:
	python3 tools/mkfont.py

clean:
	rm -rf build build-arm

.PHONY: all check fonts clean
