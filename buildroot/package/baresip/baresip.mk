################################################################################
#
# baresip: the SIP client, built as sip/build.sh builds it
#
################################################################################
# SPDX-License-Identifier: GPL-2.0-or-later

BARESIP_VERSION = 4.13.0
BARESIP_SOURCE = v$(BARESIP_VERSION).tar.gz
BARESIP_SITE = https://github.com/baresip/baresip/archive/refs/tags
BARESIP_LICENSE = BSD-3-Clause
BARESIP_LICENSE_FILES = LICENSE
BARESIP_DEPENDENCIES = libre libg722 opus openssl

BARESIP_SIP = $(BR2_EXTERNAL_T48S_PATH)/../sip

# the patches are shared with sip/build.sh
define BARESIP_APPLY_SIP_PATCHES
	$(APPLY_PATCHES) $(@D) $(BARESIP_SIP)/patches baresip-\*.patch
endef
BARESIP_POST_PATCH_HOOKS += BARESIP_APPLY_SIP_PATCHES
# one list of modules, kept in sip/build.sh
BARESIP_MODULES = $(shell sed -n 's/^MODULES="\(.*\)"$$/\1/p' $(BARESIP_SIP)/build.sh)
BARESIP_APP_MODULES = $(shell sed -n 's/^APP_MODULES="\(.*\)"$$/\1/p' $(BARESIP_SIP)/build.sh)

BARESIP_CONF_OPTS = \
	-DSTATIC=ON \
	-DMODULES="$(BARESIP_MODULES)" \
	-DAPP_MODULES_DIR=$(BARESIP_SIP)/modules \
	-DAPP_MODULES="$(BARESIP_APP_MODULES)" \
	-DLIBG722_INCLUDE_DIR=$(STAGING_DIR)/usr/include \
	-DLIBG722_LIBRARY=$(STAGING_DIR)/usr/lib/libg722.a \
	-DOPUS_INCLUDE_DIR=$(STAGING_DIR)/usr/include \
	-DOPUS_LIBRARY=$(STAGING_DIR)/usr/lib/libopus.a \
	-DRE_INCLUDE_DIR=$(STAGING_DIR)/usr/include/re \
	-DRE_LIBRARY=$(STAGING_DIR)/usr/lib/libre.a \
	-Dre_DIR=$(STAGING_DIR)/usr/lib/cmake/re \
	-DUSE_OPENSSL=ON -DUSE_MBEDTLS=OFF \
	-DOPENSSL_ROOT_DIR=$(STAGING_DIR)/usr -DOPENSSL_USE_STATIC_LIBS=ON

# only the program: the static and shared libraries are not wanted
BARESIP_BUILD_OPTS = --target baresip_exe

define BARESIP_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(BARESIP_BUILDDIR)/baresip $(TARGET_DIR)/usr/bin/baresip
	$(INSTALL) -D -m 0755 $(BARESIP_SIP)/t48-sip $(TARGET_DIR)/usr/bin/t48-sip
	$(INSTALL) -D -m 0644 $(BARESIP_SIP)/config/config $(TARGET_DIR)/usr/share/t48/sip/config
	$(INSTALL) -D -m 0644 $(BARESIP_SIP)/config/sip.example $(TARGET_DIR)/usr/share/t48/sip/sip.example
	$(INSTALL) -D -m 0644 $(@D)/LICENSE $(TARGET_DIR)/usr/share/t48/sip/licences/baresip.LICENSE
	$(INSTALL) -D -m 0644 $(LIBRE_DIR)/LICENSE $(TARGET_DIR)/usr/share/t48/sip/licences/re.LICENSE
	$(INSTALL) -D -m 0644 $(LIBG722_DIR)/LICENSE $(TARGET_DIR)/usr/share/t48/sip/licences/libg722.LICENSE
	$(INSTALL) -D -m 0644 $(OPUS_DIR)/COPYING $(TARGET_DIR)/usr/share/t48/sip/licences/opus.LICENSE
	$(INSTALL) -D -m 0644 $(LIBOPENSSL_DIR)/LICENSE.txt $(TARGET_DIR)/usr/share/t48/sip/licences/openssl.LICENSE
endef

$(eval $(cmake-package))
