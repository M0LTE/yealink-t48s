################################################################################
#
# t48-web: the web setup page, from webconfig/ in this repo
#
################################################################################
# SPDX-License-Identifier: GPL-2.0-or-later

T48_WEB_VERSION = local
T48_WEB_SITE = $(BR2_EXTERNAL_T48S_PATH)/../webconfig
T48_WEB_SITE_METHOD = local
T48_WEB_LICENSE = AGPL-3.0-or-later
T48_WEB_DEPENDENCIES = libt48
T48_WEB_GIT_VERSION = $(if $(T48_VERSION),$(T48_VERSION),$(shell git -C $(BR2_EXTERNAL_T48S_PATH) describe --always --dirty 2>/dev/null))

# the settings store, the manifest reader and the dial plan are the shell's own source files
# (shell/config.c, shell/manifest.c, shell/dialplan.c), so there is one implementation of each;
# libt48 comes from staging
define T48_WEB_BUILD_CMDS
	$(TARGET_MAKE_ENV) CFLAGS="$(TARGET_CFLAGS)" $(MAKE) -B -C $(@D) \
		CC="$(TARGET_CC)" O=build-br LIB=$(STAGING_DIR)/usr/lib/libt48.a \
		SHELL_DIR=$(BR2_EXTERNAL_T48S_PATH)/../shell \
		LIBT48_INCLUDE=$(STAGING_DIR)/usr/include \
		AUDIOD_INCLUDE=$(BR2_EXTERNAL_T48S_PATH)/../audiod/include \
		T48_VERSION="$(T48_WEB_GIT_VERSION)" \
		build-br/t48-web
endef

define T48_WEB_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build-br/t48-web $(TARGET_DIR)/usr/bin/t48-web
endef

$(eval $(generic-package))
