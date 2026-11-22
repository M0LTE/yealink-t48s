#!/bin/sh
# Prepare linux-2.6.27.47 for building modules that load into Yealink's kernel:
# vermagic "2.6.27.47 preempt mod_unload ARMv6", no symbol versions.
set -e
cd linux-2.6.27.47
make -s ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- omap_2430sdp_defconfig
sed -i 's/^CONFIG_MODVERSIONS=y/# CONFIG_MODVERSIONS is not set/; s/^CONFIG_MODULE_SRCVERSION_ALL=y/# CONFIG_MODULE_SRCVERSION_ALL is not set/' .config
yes "" | make -s ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- oldconfig >/dev/null
make -s ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- modules_prepare
