################################################################################
#
# libt48: the app library, from libt48/ in this repo
#
################################################################################
# SPDX-License-Identifier: GPL-2.0-or-later

LIBT48_VERSION = local
LIBT48_SITE = $(BR2_EXTERNAL_T48S_PATH)/../libt48
LIBT48_SITE_METHOD = local
LIBT48_LICENSE = AGPL-3.0-or-later
LIBT48_LICENSE_FILES = LICENSE
LIBT48_INSTALL_STAGING = YES

# CFLAGS from the environment: the Makefile adds its own to them
define LIBT48_BUILD_CMDS
	$(TARGET_MAKE_ENV) CFLAGS="$(TARGET_CFLAGS)" $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" AR="$(TARGET_AR)" O=build-br all
endef

define LIBT48_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/build-br/libt48.a $(STAGING_DIR)/usr/lib/libt48.a
	$(INSTALL) -D -m 0644 $(@D)/include/t48.h $(STAGING_DIR)/usr/include/t48.h
	$(INSTALL) -D -m 0644 $(@D)/include/t48_proto.h $(STAGING_DIR)/usr/include/t48_proto.h
endef

# the example app, laid out as the shell expects an installed app
define LIBT48_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build-br/examples/clock/clock \
		$(TARGET_DIR)/usr/share/t48/apps/clock/clock
	$(INSTALL) -D -m 0644 $(@D)/build-br/examples/clock/manifest.json \
		$(TARGET_DIR)/usr/share/t48/apps/clock/manifest.json
	$(INSTALL) -D -m 0644 $(@D)/build-br/examples/clock/icon.ppm \
		$(TARGET_DIR)/usr/share/t48/apps/clock/icon.ppm
endef

$(eval $(generic-package))
