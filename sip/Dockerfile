# SPDX-License-Identifier: AGPL-3.0-or-later
# Build and test environment for sip/ (issue #25): the ARM musl toolchain's
# needs, a host compiler, a SIP registrar for the call test and qemu-user to
# start the ARM binary. kamailio-tls-modules and openssl: the SIP over TLS test
# (issue #78), with a test CA made at run time. iproute2: the link test (a
# dummy link whose carrier goes down and up).
#   docker build -t t48-sip sip
#   docker run --rm -v "$PWD":/src -w /src t48-sip sh -c \
#     'native/build-toolchain.sh && sip/build.sh && TARGET=host sip/build.sh && sip/test/call-test.sh'
FROM ubuntu:26.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        clang lld llvm cmake make curl ca-certificates bzip2 xz-utils file perl patch \
        gcc-arm-none-eabi gcc libc6-dev python3 kamailio kamailio-tls-modules openssl qemu-user iproute2 \
    && rm -rf /var/lib/apt/lists/*
