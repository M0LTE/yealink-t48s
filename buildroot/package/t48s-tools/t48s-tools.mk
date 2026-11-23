################################################################################
#
# t48s-tools: our programs from native/ in this repo
#
################################################################################
# SPDX-License-Identifier: GPL-2.0-or-later

T48S_TOOLS_VERSION = local
T48S_TOOLS_SITE = $(BR2_EXTERNAL_T48S_PATH)/../native
T48S_TOOLS_SITE_METHOD = local
T48S_TOOLS_LICENSE = AGPL-3.0-or-later
T48S_TOOLS_PROGRAMS = keytest touchtest membanner cpubench waterfall

# -B: the local rsync can carry binaries left in a developer's native/
define T48S_TOOLS_BUILD_CMDS
	$(MAKE) -B -C $(@D) \
		CROSS_CC="$(TARGET_CC) $(TARGET_LDFLAGS)" \
		MKFONT=$(BR2_EXTERNAL_T48S_PATH)/../tools/mkfont.py \
		$(T48S_TOOLS_PROGRAMS)
endef

define T48S_TOOLS_INSTALL_TARGET_CMDS
	for p in $(T48S_TOOLS_PROGRAMS); do \
		$(INSTALL) -D -m 0755 $(@D)/$$p $(TARGET_DIR)/usr/bin/$$p || exit 1; \
	done
endef

$(eval $(generic-package))
