################################################################################
#
# libg722: the G.722 codec baresip's libg722 module wraps (as sip/build.sh)
#
################################################################################
# SPDX-License-Identifier: GPL-2.0-or-later

LIBG722_VERSION = 1.2.8
LIBG722_SOURCE = v$(LIBG722_VERSION).tar.gz
LIBG722_SITE = https://github.com/sippy/libg722/archive/refs/tags
LIBG722_LICENSE = BSD-2-Clause, Public Domain
LIBG722_LICENSE_FILES = LICENSE
LIBG722_INSTALL_STAGING = YES
LIBG722_INSTALL_TARGET = NO
LIBG722_HEADERS = g722.h g722_codec.h g722_encoder.h g722_decoder.h g722_common.h

define LIBG722_BUILD_CMDS
	cd $(@D) && for f in g722_encode g722_decode; do \
		$(TARGET_CC) $(TARGET_CFLAGS) -w -c $$f.c -o $$f.o || exit 1; \
	done
	rm -f $(@D)/libg722.a
	$(TARGET_AR) rcs $(@D)/libg722.a $(@D)/g722_encode.o $(@D)/g722_decode.o
endef

define LIBG722_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/libg722.a $(STAGING_DIR)/usr/lib/libg722.a
	for h in $(LIBG722_HEADERS); do \
		$(INSTALL) -D -m 0644 $(@D)/$$h $(STAGING_DIR)/usr/include/$$h || exit 1; \
	done
endef

$(eval $(generic-package))
