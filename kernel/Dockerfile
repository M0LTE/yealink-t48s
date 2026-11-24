# SPDX-License-Identifier: AGPL-3.0-or-later
# Kernel build container (gcc 4.7 for Linux 3.6.11): docker build -t t48s-kbuild:trusty kernel/
FROM ubuntu:14.04
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq gcc gcc-arm-linux-gnueabi make bc u-boot-tools xz-utils bzip2 patch python3 >/dev/null
