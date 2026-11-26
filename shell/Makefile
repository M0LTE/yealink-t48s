# SPDX-License-Identifier: AGPL-3.0-or-later
# t48-shell and the PC simulator (the same program). Plain make.
#   make            for this PC: build/t48-shell, build/t48-sim
#   make check      run the simulator tests (pictures land in build/shots/; the T46S's are t46s-*.png); the last one needs ../audiod built too.
#                   tests/small/ run as a T46S and tests/sip/ as a T48S, both with tests/small/fakesip.py for baresip
#   make CC=~/src-build/t48s/bin/armcc O=build-arm     for the phone (static, musl)
#   make LIB=/path/to/libt48.a $(O)/t48-shell   against an installed libt48 (Buildroot)
CC ?= cc
O ?= build
CFLAGS ?= -O2
# the audio service's control protocol header
AUDIOD_INCLUDE ?= ../audiod/include
CFLAGS += -std=gnu99 -Wall -Wextra -Wno-format-truncation -D_GNU_SOURCE -ffunction-sections -fdata-sections -I../libt48/include -I$(AUDIOD_INCLUDE)
LIB ?= ../libt48/$(O)/libt48.a
SRC = main.c ui.c fit.c app.c manifest.c phone.c contacts.c sipc.c audioc.c leds.c backlight.c be_fb.c be_sim.c config.c settings.c pkg.c watch.c keytest.c dialplan.c certs.c names.c
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

$(O)/t48-shell: $(SRC) shell.h zones.h dialplan.h certs.h names.h $(LIB)
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

# more for tests/15-flip.script, 16-sound.script and 17-lamps.script, which install them themselves so the other
# tests' launcher stays as it was: the flipping test, an app that speaks the protocol as level 2 did, the one that
# plays notes, the one that sets the lamps (and the same again without the permission to), and the flipping test
# again as an app that takes any size of screen (tests/small/04-apps.script)
$(O)/extra: $(LIB) tests/level2.c tests/level2.json $(wildcard ../libt48/examples/fliptest/* ../libt48/examples/tones/* ../libt48/examples/lamps/*)
	rm -rf $@ && mkdir -p $@/level2
	cp -r ../libt48/$(O)/examples/fliptest $@/fliptest
	cp -r ../libt48/$(O)/examples/tones $@/tones
	cp -r ../libt48/$(O)/examples/lamps $@/lamps
	cp -r ../libt48/$(O)/examples/lamps $@/nolamps
	sed -i -e 's/"id": "lamps"/"id": "nolamps"/' -e 's/"name": "Lamps"/"name": "No lamps"/' -e 's/\["lamps"\]/[]/' $@/nolamps/manifest.json
	cp -r ../libt48/$(O)/examples/fliptest $@/anyscreen
	sed -i -e 's/"id": "fliptest"/"id": "anyscreen"/' -e 's/"name": "Screen test"/"name": "Any screen"/' -e 's/"api": 3,/"api": 3, "screen": "any",/' $@/anyscreen/manifest.json
	$(CC) $(CFLAGS) -o $@/level2/level2 tests/level2.c
	cp tests/level2.json $@/level2/manifest.json

# feeds on disk for the app manager's tests
$(O)/feeds: $(O)/apps tests/mkfeeds.sh ../sdk/t48-pack
	sh tests/mkfeeds.sh $(O)

# apps installed by a test go in simdata/installed, settings in simdata/config
SIM = ./t48-sim --apps simdata/installed:apps --data simdata/appdata --config simdata/config --state simdata/state --access simdata/access --notice simdata/notice --apply ../tests/access/apply.sh --update ../tests/update/update.sh

# the official feed in the tests is one on disk with no apps, as the tests never reach the internet,
# and a feed that does not answer is given up on after 2 seconds rather than 15 (shell/pkg.c)
FEEDS_ENV = T48_OFFICIAL_FEED=file://$$PWD/feeds/official/index.json T48_FEED_TIMEOUT=2

# the dial plan's matcher on its own
$(O)/test_dialplan: tests/test_dialplan.c dialplan.c dialplan.h
	@mkdir -p $(O)
	$(CC) $(CFLAGS) -o $@ tests/test_dialplan.c dialplan.c

# the certificate reader on its own, with hostile files (issue #78)
$(O)/test_certs: tests/test_certs.c certs.c certs.h
	@mkdir -p $(O)
	$(CC) $(CFLAGS) -o $@ tests/test_certs.c certs.c

# failure reasons and account names on their own (shell/names.c)
$(O)/test_names: tests/test_names.c names.c names.h
	@mkdir -p $(O)
	$(CC) $(CFLAGS) -o $@ tests/test_names.c names.c

check: all $(O)/apps $(O)/extra $(O)/feeds $(O)/test_dialplan $(O)/test_certs $(O)/test_names
	$(O)/test_dialplan
	$(O)/test_certs
	$(O)/test_names
	@mkdir -p $(O)/shots
	cd $(O) && for t in ../tests/*.script; do echo "== $$t"; rm -rf simdata; \
		T48_BACKLIGHT=simdata/bl T48_INSTALL=../tests/install/t48-install T48_LIVE=simdata/live $(FEEDS_ENV) $(SIM) --script $$t 2>sim.log || { cat sim.log; exit 1; }; cat sim.log; \
		if grep -q 7355719 sim.log; then echo "the password typed in 04-settings is in the log"; exit 1; fi; done
	@# the same shell as a T46S: 480x272 at 16 bits, upside down, no touch, four soft keys, and a stand-in for the SIP service
	cd $(O) && for t in ../tests/small/*.script; do echo "== $$t"; rm -rf simdata; mkdir -p simdata/config; \
		./t48-config --config simdata/config set sip user 1001 && ./t48-config --config simdata/config set sip domain pbx.example.org || exit 1; \
		python3 ../tests/small/fakesip.py simdata/sip.sock sip:1001@pbx.example.org & fake=$$!; sleep 0.3; \
		T48_BACKLIGHT=simdata/bl T48_INSTALL=../tests/install/t48-install T48_LIVE=simdata/live $(FEEDS_ENV) $(SIM) --model t46s --sip simdata/sip.sock --script $$t 2>sim.log; r=$$?; \
		kill $$fake; cat sim.log; [ $$r = 0 ] || exit 1; done
	@# the T48S's screens again with the stand-in for the SIP service: calls that fail, and accounts by name
	cd $(O) && for t in ../tests/sip/*.script; do echo "== $$t"; rm -rf simdata; mkdir -p simdata/config; \
		./t48-config --config simdata/config set sip user 1001 && ./t48-config --config simdata/config set sip domain pbx.example.org || exit 1; \
		python3 ../tests/small/fakesip.py simdata/sip.sock sip:1001@pbx.example.org & fake=$$!; sleep 0.3; \
		T48_BACKLIGHT=simdata/bl $(FEEDS_ENV) $(SIM) --sip simdata/sip.sock --script $$t 2>sim.log; r=$$?; \
		kill $$fake; cat sim.log; [ $$r = 0 ] || exit 1; done
	sh tests/feeds-trust.sh $(O)
	sh tests/sound/sound-test.sh $(O)
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
