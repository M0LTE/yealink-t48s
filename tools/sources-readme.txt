Source for the binaries of yealink-t48s @VERSION@
==================================================

This archive goes with the release files built from commit
@COMMIT@ of https://github.com/M0LTE/yealink-t48s
(t48s-stick.zip, t48s-live.tar.gz, Image-kexec, Image-kexec-linux3.6,
T48S-kernel.uimg, system.img, rootfs.cpio.gz, t48s-sdk.tar.gz).

yealink-t48s-@VERSION@.tar.gz
    The repository at that commit: our own programs (AGPL-3.0-or-later),
    the kernel drivers, device trees, patches and configurations
    (GPL-2.0; kernel/, kernel6/, kmod/), the Buildroot external tree
    (buildroot/) and the build scripts. NOTICE in it lists the
    third-party code and its licences.

linux/
    The kernel source archives exactly as downloaded from kernel.org:
    Linux 6.18 (the phone's kernel; built by kernel6/build.sh with
    kernel6/patches and kernel6/drivers) and Linux 3.6.11 (the fallback
    kernel and the C library's kernel headers; built by kernel/build.sh
    with kernel/drivers). While the headers come from 3.6.11 its archive
    is not repeated here: it is in
    buildroot/legal-info/sources/linux-headers-3.6.11/.

buildroot/buildroot-*.tar.xz
    The Buildroot release that built the root filesystem and the SDK.

buildroot/legal-info/
    Buildroot's "make legal-info" for this build. sources/ holds the
    source archive and patches of every package in the phone's image
    (busybox, dropbear, curl, mbedTLS, baresip, re, libg722, musl, the
    GCC runtime and ours); host-sources/ the same for the tools that
    built it, which are also what the SDK contains (GCC, binutils and
    the rest). licenses/ and host-licenses/ hold the licence texts,
    manifest.csv and host-manifest.csv list package, version, licence
    and where the source came from, buildroot.config is the
    configuration. Its README says what Buildroot could not save.

To rebuild: docs/developing.md in the repository archive, or
.github/workflows/build.yml, which is the recipe.
