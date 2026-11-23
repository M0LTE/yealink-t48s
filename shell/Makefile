# SPDX-License-Identifier: AGPL-3.0-or-later
# t48-shell and the PC simulator (the same program). Plain make.
#   make            for this PC: build/t48-shell, build/t48-sim
#   make check      run the simulator tests (pictures land in build/shots/)
#   make CC=~/src-build/t48s/bin/armcc O=build-arm     for the phone (static, musl)
#   make LIB=/path/to/libt48.a $(O)/t48-shell   against an installed libt48 (Buildroot)
CC ?= cc
O ?= build
CFLAGS ?= -O2
# the audio service's control protocol header
AUDIOD_INCLUDE ?= ../audiod/include
CFLAGS += -std=gnu99 -Wall -Wextra -Wno-format-truncation -D_GNU_SOURCE -ffunction-sections -fdata-sections -I../libt48/include -I$(AUDIOD_INCLUDE)
LIB ?= ../libt48/$(O)/libt48.a
SRC = main.c app.c manifest.c phone.c sipc.c audioc.c leds.c backlight.c be_fb.c be_sim.c config.c settings.c pkg.c
# what About shows; Buildroot passes it in, as its copy of the source has no git
T48_VERSION ?= $(shell git describe --always --dirty 2>/dev/null)
ifneq ($(T48_VERSION),)
CFLAGS += -DT48_VERSION='"$(T48_VERSION)"'
endif

all: $(O)/t48-shell $(O)/t48-sim

# build ../libt48 unless LIB was given
ifeq ($(origin LIB),file)
$(LIB): FORCE
	$(MAKE) -C ../libt48 CC="$(CC)" O=$(O)
endif

$(O)/t48-shell: $(SRC) shell.h $(LIB)
	@mkdir -p $(O)
	$(CC) $(CFLAGS) -Wl,--gc-sections -o $@ $(SRC) $(LIB)

# the same program under its other names: the simulator, the app manager, the settings tool
$(O)/t48-sim: $(O)/t48-shell
	ln -sf t48-shell $@
	ln -sf t48-shell $(O)/t48-app
	ln -sf t48-shell $(O)/t48-config

# a folder of installed apps for the tests: the example clock and a badly behaved one
$(O)/apps: $(LIB) tests/crasher.c tests/crasher.json
	rm -rf $@ && mkdir -p $@/crasher
	cp -r ../libt48/$(O)/examples/clock $@/clock
	$(CC) $(CFLAGS) -Wl,--gc-sections -o $@/crasher/crasher tests/crasher.c $(LIB)
	cp tests/crasher.json $@/crasher/manifest.json

# feeds on disk for the app manager's tests
$(O)/feeds: $(O)/apps tests/mkfeeds.sh ../sdk/t48-pack
	sh tests/mkfeeds.sh $(O)

# apps installed by a test go in simdata/installed, settings in simdata/config
SIM = ./t48-sim --apps simdata/installed:apps --data simdata/appdata --config simdata/config --state simdata/state --access simdata/access --apply ../tests/access/apply.sh

check: all $(O)/apps $(O)/feeds
	@mkdir -p $(O)/shots
	cd $(O) && for t in ../tests/*.script; do echo "== $$t"; rm -rf simdata; \
		T48_BACKLIGHT=simdata/bl $(SIM) --script $$t 2>sim.log || { cat sim.log; exit 1; }; cat sim.log; \
		if grep -q 7355719 sim.log; then echo "the password typed in 04-settings is in the log"; exit 1; fi; done
	@echo "all simulator tests passed"

# the same against the real feed on the internet: installs hello, links and
# waterfall, and starts them if RUNNER (such as qemu-arm) is given, as they
# are built for the phone
check-live: all $(O)/apps
	@mkdir -p $(O)/shots
	cd $(O) && rm -rf simdata && $(SIM) --script ../tests/live/install.script
	$(if $(RUNNER),cd $(O) && $(SIM) --runner $(RUNNER) --script ../tests/live/run.script)

clean:
	rm -rf build build-arm
	$(MAKE) -C ../libt48 clean

FORCE:
.PHONY: all check check-live clean FORCE
