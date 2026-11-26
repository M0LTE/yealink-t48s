################################################################################
#
# libre: baresip's base library (as sip/build.sh)
#
################################################################################
# SPDX-License-Identifier: GPL-2.0-or-later

LIBRE_VERSION = 4.13.0
LIBRE_SOURCE = v$(LIBRE_VERSION).tar.gz
LIBRE_SITE = https://github.com/baresip/re/archive/refs/tags
LIBRE_LICENSE = BSD-3-Clause
LIBRE_LICENSE_FILES = LICENSE
LIBRE_INSTALL_STAGING = YES
LIBRE_INSTALL_TARGET = NO
LIBRE_DEPENDENCIES = openssl

# the patches are shared with sip/build.sh
define LIBRE_APPLY_SIP_PATCHES
	$(APPLY_PATCHES) $(@D) $(BR2_EXTERNAL_T48S_PATH)/../sip/patches re-\*.patch
endef
LIBRE_POST_PATCH_HOOKS += LIBRE_APPLY_SIP_PATCHES

# OpenSSL: SIP over TLS, SRTP, and the hashes for digest login and STUN (issue #78)
LIBRE_CONF_OPTS = \
	-DLIBRE_BUILD_SHARED=OFF -DLIBRE_BUILD_STATIC=ON \
	-DUSE_OPENSSL=ON -DUSE_MBEDTLS=OFF \
	-DOPENSSL_ROOT_DIR=$(STAGING_DIR)/usr -DOPENSSL_USE_STATIC_LIBS=ON \
	-DUSE_BFCP=OFF -DUSE_PCP=OFF -DUSE_RTMP=OFF -DUSE_TRACE=OFF \
	-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON \
	-DCMAKE_DISABLE_FIND_PACKAGE_Backtrace=ON

$(eval $(cmake-package))
