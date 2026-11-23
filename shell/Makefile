# SPDX-License-Identifier: AGPL-3.0-or-later
# t48-shell and the PC simulator (the same program). Plain make.
#   make            for this PC: build/t48-shell, build/t48-sim
#   make check      run the simulator tests (pictures land in build/shots/)
#   make CC=~/src-build/t48s/bin/armcc O=build-arm     for the phone (static, musl)
CC ?= cc
O ?= build
CFLAGS ?= -O2
CFLAGS += -std=gnu99 -Wall -Wextra -Wno-format-truncation -D_GNU_SOURCE -ffunction-sections -fdata-sections -I../libt48/include
LIB = ../libt48/$(O)/libt48.a
SRC = main.c app.c manifest.c be_fb.c be_sim.c

all: $(O)/t48-shell $(O)/t48-sim

$(LIB): FORCE
	$(MAKE) -C ../libt48 CC="$(CC)" O=$(O)

$(O)/t48-shell: $(SRC) shell.h $(LIB)
	@mkdir -p $(O)
	$(CC) $(CFLAGS) -Wl,--gc-sections -o $@ $(SRC) $(LIB)

$(O)/t48-sim: $(O)/t48-shell
	ln -sf t48-shell $@

# a folder of installed apps for the tests: the example clock and a badly behaved one
$(O)/apps: $(LIB) tests/crasher.c tests/crasher.json
	rm -rf $@ && mkdir -p $@/crasher
	cp -r ../libt48/$(O)/examples/clock $@/clock
	$(CC) $(CFLAGS) -Wl,--gc-sections -o $@/crasher/crasher tests/crasher.c $(LIB)
	cp tests/crasher.json $@/crasher/manifest.json

check: all $(O)/apps
	@mkdir -p $(O)/shots
	cd $(O) && for t in ../tests/*.script; do echo "== $$t"; ./t48-sim --apps apps --script $$t || exit 1; done
	@echo "all simulator tests passed"

clean:
	rm -rf build build-arm
	$(MAKE) -C ../libt48 clean

FORCE:
.PHONY: all check clean FORCE
