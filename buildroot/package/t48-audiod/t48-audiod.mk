################################################################################
#
# t48-audiod: the audio service, from audiod/ in this repo
#
################################################################################
# SPDX-License-Identifier: GPL-2.0-or-later

T48_AUDIOD_VERSION = local
T48_AUDIOD_SITE = $(BR2_EXTERNAL_T48S_PATH)/../audiod
T48_AUDIOD_SITE_METHOD = local
T48_AUDIOD_LICENSE = AGPL-3.0-or-later, BSD-3-Clause (speexdsp echo canceller)
T48_AUDIOD_LICENSE_FILES = third_party/speexdsp/COPYING

# the call stream's header lives with baresip's module in sip/modules
define T48_AUDIOD_BUILD_CMDS
	$(TARGET_MAKE_ENV) CFLAGS="$(TARGET_CFLAGS)" $(MAKE) -B -C $(@D) \
		CC="$(TARGET_CC)" O=build-br \
		T48AUDIO=$(BR2_EXTERNAL_T48S_PATH)/../sip/modules/t48audio all
endef

define T48_AUDIOD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build-br/t48-audiod $(TARGET_DIR)/usr/bin/t48-audiod
	$(INSTALL) -D -m 0755 $(@D)/build-br/t48-audioctl $(TARGET_DIR)/usr/bin/t48-audioctl
endef

$(eval $(generic-package))
