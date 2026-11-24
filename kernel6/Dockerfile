# SPDX-License-Identifier: AGPL-3.0-or-later
# The build container for kernel6/ (a current long-term kernel): Debian 13's own
# ARM cross compiler (gcc 14), nothing unusual; and QEMU for qemu-test.sh.
#   docker build -t t48s-kbuild6:trixie kernel6
# CI builds it without QEMU (--build-arg QEMU=), which it does not use.
FROM debian:trixie
ARG QEMU=qemu-system-arm
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
    gcc libc6-dev gcc-arm-linux-gnueabi make bc bison flex libssl-dev libelf-dev xz-utils cpio python3 \
    kmod u-boot-tools device-tree-compiler patch ca-certificates curl $QEMU \
    && rm -rf /var/lib/apt/lists/*
