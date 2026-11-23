# Buildroot: the phone's root filesystem

Issue [#16](https://github.com/M0LTE/yealink-t48s/issues/16). A Buildroot external tree that builds the root filesystem built into the kexec kernel image, and the SDK. It is the only root filesystem: the hand-made one it replaced is gone.

```
buildroot/build.sh WORKDIR          # root filesystem: WORKDIR/output/images/rootfs.cpio
buildroot/build.sh WORKDIR sdk      # Buildroot's SDK tarball, also in output/images
```

`sdk/assemble.sh` turns Buildroot's SDK into the app SDK by adding the simulator, the packaging tool and the make rules; CI publishes it ([sdk/README.md](../sdk/README.md)).

`build.sh` downloads the pinned Buildroot release (the 2026.02.x long-term line), checks its SHA-256 and builds `t48s_defconfig`. Set `BR2_DL_DIR` and `BR2_CCACHE_DIR` to share downloads and the compiler cache between builds. The host needs Buildroot's usual packages and nothing else. A first build takes about half an hour on 8 cores.

To boot it, build it into the kernel (`kernel/README.md`), then `tools/ourkernel.sh boot Image-kexec`.

## What is in it

musl, ARM1176 soft-float, Linux 3.6 headers, everything static.

- busybox (init, DHCP, NTP from the DHCP servers), dropbear (key login only), curl with mbedTLS and a CA bundle, mtd-utils.
- `t48-shell` on the screen at boot, with the clock app in `/usr/share/t48/apps` (packages `t48-shell`, `libt48`). libt48's static library and headers are in staging, so the SDK has them.
- Our tools from `native/` (package `t48s-tools`): keytest, touchtest, membanner, cpubench, waterfall, t48rec, t48mix.
- `t48-audiod` and `t48-audioctl` (package `t48-audiod`). Installed, not started.
- baresip with `t48-sip`, the config and the `sip.example` settings template in `/usr/share/t48/sip` (packages `libre`, `libg722`, `baresip`, built as `sip/build.sh` builds them). Installed, not started.

## Files

| | |
|---|---|
| `configs/t48s_defconfig` | the configuration |
| `package/` | our packages; the local ones build straight from this repo's folders |
| `board/t48s/rootfs-overlay/` | `/etc/inittab`, `/etc/init.d/rcS` (mounts, watchdog off, network, SSH), `/etc/pointercal`, `back-to-yealink` |
| `board/t48s/post-build.sh` | drops the stock init scripts, makes the `/data` folders |

The screen fonts for `native/` are committed in `native/gen/`; `make -C native fonts` regenerates them (needs python3-pil and the DejaVu fonts).

SSH keys never go into this output. `kernel/build.sh` adds them, for our own test builds only.
