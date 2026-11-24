# Buildroot: the phone's root filesystem

Issue [#16](https://github.com/M0LTE/yealink-t48s/issues/16). A Buildroot external tree that builds the two files a boot is made of, the first stage and the system image ([docs/boot.md](../docs/boot.md)), and the SDK. It is the only root filesystem: the hand-made one it replaced is gone.

```
buildroot/build.sh WORKDIR          # WORKDIR/output/images: system.img, firststage.cpio (and rootfs.cpio)
buildroot/build.sh WORKDIR sdk      # Buildroot's SDK tarball, also in output/images
```

`sdk/assemble.sh` turns Buildroot's SDK into the app SDK by adding the simulator, the packaging tool and the make rules; CI publishes it ([sdk/README.md](../sdk/README.md)).

`build.sh` downloads the pinned Buildroot release (the 2026.02.x long-term line), checks its SHA-256 and builds `t48s_defconfig`. Set `BR2_DL_DIR` and `BR2_CCACHE_DIR` to share downloads and the compiler cache between builds. The host needs Buildroot's usual packages and nothing else. A first build takes about half an hour on 8 cores.

To boot it, build it into the kernel ([`kernel6/README.md`](../kernel6/README.md)), then `tools/ourkernel.sh boot Image-kexec`.

## What is in it

musl, ARM1176 soft-float, Linux 3.6 headers, everything static. The phone's kernel is Linux 6.18 (`kernel6/`) since issue #55; the headers stay at 3.6 for one more release so that the same files also run on the 3.6 fallback (`kernel/`), which CI still builds from them. The one program that cares which kernel it is on, `t48-watchdog`, finds out when it starts.

- busybox (init, DHCP, NTP from the DHCP servers), dropbear (off until the owner turns it on at the screen, key login only: [docs/access.md](../docs/access.md)), curl with mbedTLS and a CA bundle. runit from busybox supervises the services. busybox's UBI tools for `/data` in flash ([docs/flash.md](../docs/flash.md)); no mtd-utils and no raw flash applets, and the kernel driver writes only our own range.
- `t48-shell` on the screen at boot, with the clock app in `/usr/share/t48/apps` (packages `t48-shell`, `libt48`). The same program is `t48-app` ([docs/apps.md](../docs/apps.md)) and `t48-config` ([docs/settings.md](../docs/settings.md)); `t48-apply` makes the running phone follow a changed settings file. libt48's static library and headers are in staging, so the SDK has them.
- `t48-web`, the web setup page (package `t48-web`, from `webconfig/`; [docs/web.md](../docs/web.md)): a runit service that stays down until the owner turns it on at the screen.
- Our tools from `native/` (package `t48s-tools`): keytest, touchtest, membanner, cpubench, t48rec, t48play, t48mix, nandread (read-only flash dumps, `docs/nand.md`). The waterfall is an app now ([M0LTE/yealink-t48s-apps](https://github.com/M0LTE/yealink-t48s-apps)).
- `t48-audiod` and `t48-audioctl` (package `t48-audiod`), started at boot under runit.
- baresip with `t48-sip`, the config and the `sip.example` settings template in `/usr/share/t48/sip` (packages `libre`, `libg722`, `baresip`, built as `sip/build.sh` builds them). Started at boot under runit once there is an account.

## Files

| | |
|---|---|
| `configs/t48s_defconfig` | the configuration |
| `package/` | our packages; the local ones build straight from this repo's folders |
| `board/t48s/rootfs-overlay/` | `/etc/inittab`, `/etc/init.d/rcS` (mounts, `/data`, network, `t48-access boot`, `t48-live boot`), `/etc/service` (the runit services), `/etc/pointercal`, `back-to-yealink`, `t48-apply`, `t48-access`, `t48-services`, `t48-data` |
| `board/t48s/rootfs-overlay/usr/bin/t48-live` | run by `rcS`; acts only when the phone was started from a USB stick or TFTP: clears U-Boot's flag through the kernel and leaves the "nothing is installed" note for the idle screen ([docs/live-image.md](../docs/live-image.md)) |
| `board/t48s/firststage/`, `mkfirststage.py`, `post-image.sh` | the first stage and how it and `system.img` are made |
| `board/t48s/post-build.sh` | drops the stock init scripts, makes the `/data` folders, links `/root/.ssh` to `/data/access/ssh` |

The screen fonts for `native/` are committed in `native/gen/`; `make -C native fonts` regenerates them (needs python3-pil and the DejaVu fonts).

SSH keys never go into this output, and an image made from it alone starts with SSH off. `kernel6/build.sh` (and `kernel/build.sh` for the fallback) adds our keys for our own test builds only, which also turns SSH on in them ([docs/access.md](../docs/access.md#our-own-test-builds)).

Two things about HTTPS on this image, both found when the app manager first tried it (2026-11-23). curl is built with no default CA bundle, so `t48-app` and `/etc/profile` point it at `/etc/ssl/certs/ca-certificates.crt`. And Linux 3.6's `/dev/random` blocks for want of entropy, which hung curl for ever, so `rcS` replaces it with the non-blocking pool after stirring in what differs between phones and boots; that is weak just after boot, and a seed kept across boots needs issue #29.
