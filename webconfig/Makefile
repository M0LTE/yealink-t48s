# SPDX-License-Identifier: AGPL-3.0-or-later
# t48-web: the phone's web setup page (docs/web.md). Plain make.
#   make            for this PC: build/t48-web
#   make check      the tests: the handlers over HTTP, then the page and the
#                   shell's simulator together (pictures land in build/shots/)
#   make shots      pictures of the pages for the docs (needs Playwright)
#   make CC=~/src-build/t48s/bin/armcc O=build-arm     for the phone (static, musl)
#   make LIB=/path/to/libt48.a $(O)/t48-web   against an installed libt48 (Buildroot)
CC ?= cc
O ?= build
CFLAGS ?= -O2
SHELL_DIR ?= ../shell
AUDIOD_INCLUDE ?= ../audiod/include
LIBT48_INCLUDE ?= ../libt48/include
CFLAGS += -std=gnu99 -Wall -Wextra -Wno-format-truncation -D_GNU_SOURCE -ffunction-sections -fdata-sections \
	-I. -I$(SHELL_DIR) -I$(LIBT48_INCLUDE) -I$(AUDIOD_INCLUDE)
LIB ?= ../libt48/$(O)/libt48.a
# the settings store, the reader of app manifests and the dial plan's checker are the shell's own files:
# one implementation of each
SRC = http.c pages.c wav.c $(O)/assets.c $(SHELL_DIR)/config.c $(SHELL_DIR)/manifest.c $(SHELL_DIR)/dialplan.c $(SHELL_DIR)/certs.c
T48_VERSION ?= $(shell git describe --always --dirty 2>/dev/null)
ifneq ($(T48_VERSION),)
CFLAGS += -DT48_VERSION='"$(T48_VERSION)"'
endif

all: $(O)/t48-web

ifeq ($(origin LIB),file)
$(LIB): FORCE
	$(MAKE) -C ../libt48 CC="$(CC)" O=$(O)
endif

# the style sheet (every theme) and two faces of each theme's font (libt48/tools/mkwebfont.py makes those)
FONTS = fonts/b612-regular.woff2 fonts/b612-bold.woff2 fonts/plex-regular.woff2 fonts/plex-bold.woff2 \
	fonts/figtree-regular.woff2 fonts/figtree-bold.woff2

$(O)/assets.c: style.css t48.js $(FONTS) mkassets.sh
	@mkdir -p $(O)
	sh mkassets.sh style.css t48.js $(FONTS) > $@

$(O)/t48-web: $(SRC) web.h $(SHELL_DIR)/shell.h $(SHELL_DIR)/zones.h $(SHELL_DIR)/certs.h $(LIB)
	@mkdir -p $(O)
	$(CC) $(CFLAGS) -Wl,--gc-sections -o $@ $(SRC) $(LIB) -lm

# the shell's simulator and t48-config, for the tests
shell: FORCE
	$(MAKE) -C $(SHELL_DIR) O=$(O) all $(O)/apps $(O)/feeds

check: all shell
	python3 tests/test_web.py $(O)
	sh tests/with-phone.sh $(O)
	@echo "all web tests passed"

shots: all shell
	sh tests/shots.sh $(O)

clean:
	rm -rf build build-arm

FORCE:
.PHONY: all shell check shots clean FORCE
