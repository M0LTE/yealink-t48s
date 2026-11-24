################################################################################
#
# t48-shell: the shell, from shell/ in this repo
#
################################################################################
# SPDX-License-Identifier: GPL-2.0-or-later

T48_SHELL_VERSION = local
T48_SHELL_SITE = $(BR2_EXTERNAL_T48S_PATH)/../shell
T48_SHELL_SITE_METHOD = local
T48_SHELL_LICENSE = AGPL-3.0-or-later
T48_SHELL_DEPENDENCIES = libt48
# what Settings, About shows: the copy of the source Buildroot builds from has no git
T48_SHELL_GIT_VERSION = $(if $(T48_VERSION),$(T48_VERSION),$(shell git -C $(BR2_EXTERNAL_T48S_PATH) describe --always --dirty 2>/dev/null))

# linked against the libt48 in staging (LIB=...), not a copy built here; the
# audio service's control header comes from audiod/include
define T48_SHELL_BUILD_CMDS
	$(TARGET_MAKE_ENV) CFLAGS="$(TARGET_CFLAGS)" $(MAKE) -B -C $(@D) \
		CC="$(TARGET_CC)" O=build-br LIB=$(STAGING_DIR)/usr/lib/libt48.a \
		AUDIOD_INCLUDE=$(BR2_EXTERNAL_T48S_PATH)/../audiod/include \
		T48_VERSION="$(T48_SHELL_GIT_VERSION)" \
		build-br/t48-shell
endef

define T48_SHELL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build-br/t48-shell $(TARGET_DIR)/usr/bin/t48-shell
	ln -sf t48-shell $(TARGET_DIR)/usr/bin/t48-app
	ln -sf t48-shell $(TARGET_DIR)/usr/bin/t48-config
	mkdir -p $(TARGET_DIR)/usr/share/t48/apps
endef

$(eval $(generic-package))
