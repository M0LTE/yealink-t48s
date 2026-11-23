#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# The musl cross toolchain for the T48S (ARMv6, EABI soft-float), as used by
# native/Makefile and sip/build.sh: musl 1.2.5 built with clang into
# $T/sysroot, linked with the ARM libgcc from arm-none-eabi-gcc (ARMv6 has no
# hardware divide), with the 3.6.11 kernel headers, wrapped as $T/bin/armcc.
# Stand-in until Buildroot (#16).
# Needs: clang, lld, llvm-ar, arm-none-eabi-gcc, curl.
set -e
T=${T:-$HOME/src-build/t48s}
MUSL=musl-1.2.5
LLVM_AR=$(command -v llvm-ar || command -v llvm-ar-21)
LLVM_RANLIB=$(command -v llvm-ranlib || command -v llvm-ranlib-21)
TGT="--target=armv6-linux-musleabi -march=armv6 -mfloat-abi=soft -marm"
mkdir -p "$T" && cd "$T"

[ -f $MUSL.tar.gz ] || curl -fsSLO https://musl.libc.org/releases/$MUSL.tar.gz
echo "a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4  $MUSL.tar.gz" | sha256sum -c

if [ ! -f sysroot/lib/libc.a ]; then
    rm -rf $MUSL && tar xf $MUSL.tar.gz && cd $MUSL
    CC="clang $TGT" AR=$LLVM_AR RANLIB=$LLVM_RANLIB \
        ./configure --target=arm-linux-musleabi --prefix="$T/sysroot" --disable-shared
    make -j"$(nproc)" && make install
    cd "$T"
fi

# Kernel headers from the kernel we run (netlink and friends, for libre)
LINUX=linux-3.6.11
if [ ! -f sysroot/include/linux/netlink.h ]; then
    [ -f $LINUX.tar.xz ] || curl -fsSLO https://cdn.kernel.org/pub/linux/kernel/v3.x/$LINUX.tar.xz
    echo "b0208020e64cb79f05ec40b9d9cf6ff57be3bc459062871f21b4f5fe346ccb32  $LINUX.tar.xz" | sha256sum -c
    rm -rf $LINUX && tar xf $LINUX.tar.xz
    # headers_install empties INSTALL_HDR_PATH/include, so not straight into the sysroot
    make -C $LINUX ARCH=arm HOSTCFLAGS="-std=gnu89 -w" INSTALL_HDR_PATH="$T/linux-headers" headers_install >/dev/null
    cp -r linux-headers/include/. sysroot/include/
    rm -rf $LINUX linux-headers
fi

# Empty C++ startup files so clang's link line is satisfied
mkdir -p crtstub && echo "" > crtstub/e.c
for f in crtbeginT crtbegin crtend; do
    clang $TGT -c crtstub/e.c -o crtstub/$f.o
done

LIBGCC_DIR=$(dirname "$(arm-none-eabi-gcc -march=armv6 -mfloat-abi=soft -marm -print-libgcc-file-name)")
mkdir -p bin
cat > bin/armcc <<EOW
#!/bin/sh
exec clang $TGT \\
  --sysroot=$T/sysroot -isystem $T/sysroot/include -fuse-ld=lld -static \\
  -rtlib=libgcc --unwindlib=none -B$T/crtstub -L$LIBGCC_DIR "\$@"
EOW
chmod +x bin/armcc
echo "$T/bin/armcc ready"
