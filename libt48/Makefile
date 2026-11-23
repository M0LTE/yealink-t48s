# SPDX-License-Identifier: AGPL-3.0-or-later
# libt48: the app library. Plain make.
#   make                                   for this PC, into build/
#   make CC=~/src-build/t48s/bin/armcc O=build-arm     for the phone (static, musl)
CC ?= cc
AR ?= ar
O ?= build
CFLAGS ?= -O2
CFLAGS += -std=gnu99 -Wall -Wextra -Wno-format-truncation -D_GNU_SOURCE -ffunction-sections -fdata-sections -Iinclude

OBJ = $(O)/draw.o $(O)/image.o $(O)/json.o $(O)/app.o $(O)/fonts_gen.o

all: $(O)/libt48.a $(O)/examples/clock/clock

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

# regenerate the committed font tables (needs python3-pil and the DejaVu fonts)
fonts:
	python3 tools/mkfont.py src/fonts_gen.c

clean:
	rm -rf build build-arm

.PHONY: all fonts clean
