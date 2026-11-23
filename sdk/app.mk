# SPDX-License-Identifier: AGPL-3.0-or-later
# Build rules for a T48 app, shipped in the SDK as share/t48/app.mk.
#
# An app's Makefile:
#     APP = hello          # the app's id: also its folder and program name
#     SRC = hello.c
#     include $(T48_SDK)/share/t48/app.mk
#
#   make            the phone's app folder in build/arm/APP and the same for
#                   this PC (for the simulator) in build/host/APP
#   make package    build/APP-VERSION.t48app, from the phone's folder
#   make shot       start the app in the simulator, save build/shot.png
#   make check      run each tests/*.script in the simulator, from this folder,
#                   with the app started (settings: tests/APP.conf if there is one)
#   make clean
#
# Optional: EXTRA (more files for the app folder), LDLIBS, CFLAGS, O (build folder).
ifndef T48_SDK
$(error T48_SDK is not set: point it at the unpacked SDK, see its README)
endif
T48_SHARE := $(T48_SDK)/share/t48
ARM_CC ?= $(T48_SDK)/bin/arm-buildroot-linux-musleabi-gcc
HOST_CC ?= cc
O ?= build
T48_FLAGS := -std=gnu99 -Wall -Wextra -Wno-format-truncation -D_GNU_SOURCE -ffunction-sections -fdata-sections
FILES := manifest.json $(wildcard icon.ppm) $(EXTRA)
DEPS := $(SRC) $(wildcard *.h) $(FILES) Makefile
HOST_LIB := $(abspath $(O))/host-libt48/libt48.a
SIM := $(T48_SDK)/bin/t48-sim --apps $(O)/host --data $(O)/sim --config $(O)/sim --state $(O)/sim

all: $(O)/arm/$(APP)/$(APP) $(O)/host/$(APP)/$(APP)

# the phone: static, stripped, against the SDK's libt48
$(O)/arm/$(APP)/$(APP): $(DEPS)
	@mkdir -p $(@D)
	$(ARM_CC) -Os $(T48_FLAGS) $(CFLAGS) -static -s -Wl,--gc-sections -o $@ $(SRC) -lt48 $(LDLIBS) -lm
	cp $(FILES) $(@D)/

# this PC, for the simulator: libt48 built from the source in the SDK
$(O)/host/$(APP)/$(APP): $(DEPS) $(HOST_LIB)
	@mkdir -p $(@D)
	$(HOST_CC) -O2 -g $(T48_FLAGS) $(CFLAGS) -I$(T48_SHARE)/libt48/include -o $@ $(SRC) $(HOST_LIB) $(LDLIBS) -lm
	cp $(FILES) $(@D)/

$(HOST_LIB):
	$(MAKE) -C $(T48_SHARE)/libt48 CC=$(HOST_CC) O=$(patsubst %/,%,$(dir $(HOST_LIB))) $(HOST_LIB)

package: $(O)/arm/$(APP)/$(APP)
	$(T48_SDK)/bin/t48-pack $(O)/arm/$(APP) $(O)

shot: $(O)/host/$(APP)/$(APP)
	rm -rf $(O)/sim && mkdir -p $(O)/sim/apps && if [ -f tests/$(APP).conf ]; then cp tests/$(APP).conf $(O)/sim/apps/; fi
	$(SIM) --app $(APP) --shot $(O)/shot.png

check: $(O)/host/$(APP)/$(APP)
	@for t in tests/*.script; do [ -f "$$t" ] || continue; echo "== $$t"; rm -rf $(O)/sim; \
		mkdir -p $(O)/sim/apps; if [ -f tests/$(APP).conf ]; then cp tests/$(APP).conf $(O)/sim/apps/; fi; \
		$(SIM) --app $(APP) --script $$t || exit 1; done

clean:
	rm -rf $(O)

.PHONY: all package shot check clean
