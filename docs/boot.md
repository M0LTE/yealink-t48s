# How the phone starts

Issue [#17](https://github.com/M0LTE/yealink-t48s/issues/17). Design: [architecture.md](architecture.md#1-userspace-base).

The kernel is Linux 6.18 ([`kernel6/`](../kernel6/README.md)) since 2026-11-24 (issue [#55](https://github.com/M0LTE/yealink-t48s/issues/55)). Linux 3.6 ([`kernel/`](../kernel/README.md)) is still built from the same two files as a fallback, `Image-kexec-linux3.6`, for one release. Everything below is the same on both.

A boot is made of two files, both from one Buildroot run:

| File | What | Size |
|---|---|---|
| `firststage.cpio` | the first stage: busybox, dropbear and one script. Built into the kernel image | about 1 MB |
| `system.img` | the system image: the whole root filesystem as squashfs, read-only | about 4 MB |

```
 kernel starts
   -> first stage (/init, buildroot/board/t48s/firststage/init)
        finds the system image, mounts it read-only, puts /run and /tmp in RAM
        nothing usable?  -> stays up as RESCUE
   -> the system image's own init
        rcS: /data (flash or RAM), network, SSH
        runit: shell, audio service, baresip, watchdog, USB sticks
```

## Where the system image comes from

The first stage looks, in this order:

1. **In RAM beside the kernel**, as the file `/system.img`. That is how a kexec boot works (`kernel6/build.sh` with `SYSTEM=system.img` packs it next to the first stage; `kernel/build.sh kexec` does the same for the 3.6 fallback), and a recovery-mode (live) boot too: U-Boot loads a second file as a ramdisk, `T48S.rfs`, and that is a cpio archive holding `system.img`, which lands in the same place ([live-image.md](live-image.md#what-is-in-the-two-files-and-why)).
2. **In flash**, a system slot, once the phone is installed (issues #29 and #31, [install.md](install.md)). The kernel then says `t48boot=flash`; the first stage attaches UBI on the `app` partition, picks slot A or B (the active one, or a new one on its single try), and copies that image into RAM. Written and rehearsed off the phone; not yet run on it.

Either way it is the same file, mounted the same way (squashfs through a loop device), so there is one system image however the phone was started. It costs its own size in RAM, about 4 MB, against 7 MB when everything was unpacked.

The system image is read-only. What changes lives elsewhere: `/run` and `/tmp` in RAM (logs are in `/tmp`), `/data` for everything the owner sets (in RAM on a live boot, always). The two files in `/etc` that change at run time are really in `/run`: `resolv.conf` is a link, and a copy of `shadow` is mounted over the read-only one (the Access page's password door sets root's password for ten minutes). Our own test builds' SSH keys ride beside the first stage and are mounted on `/etc/t48-dev`.

## Rescue

If there is no system image, or it does not mount, the first stage stays up by itself:

- a message on the screen saying why, with the phone's address;
- the network by DHCP, and SSH as root: with the built-in keys on one of our test builds, otherwise with a password made up at that moment and shown only on the screen;
- a shell on the serial port. After a start from U-Boot it needs the serial port's interrupt, which the SoC sends to its secure side until told otherwise: the kernel does that (`kernel6/drivers/t48s_secure.c`, issue #59), and `rcS` does it for kernels built before 2026-11-25.

On an installed phone it is also where a start ends when neither slot starts, or when an install was cut short; the installer can be run again from the PC against it. To see it: build a kernel with the first stage alone (`ROOTFS=firststage.cpio` and no `SYSTEM`) and boot it. A live boot whose ramdisk is missing or damaged ends here too. On an installed phone the rescue menu comes first: when no slot starts, and when X is held at power-on ([install.md](install.md#rescue), issue #32). The menu is `firststage/menu.sh`, with `t48-keys` reading the keypad.

## Services

`runsvdir` (busybox runit) keeps these programs running and starts any of them again if it stops:

| Service | Runs | Waits for |
|---|---|---|
| `shell` | `t48-shell`: the screen, keys, hook | nothing. It finds the other two whenever they appear and looks again if they go |
| `audiod` | `t48-audiod --backend alsa` | nothing |
| `sip` | `t48-sip` (baresip) | an account in `/data/config/sip`, the audio service, an address |
| `watchdog` | `t48-watchdog` | nothing |
| `usbmount` | `t48-usbmount watch`: USB sticks read-only on `/run/media` for apps ([app-protocol.md](app-protocol.md#what-an-app-is-given)) | nothing |
| `update-check` | `t48-update daily`: the daily check for firmware and app updates, which installs nothing ([install.md](install.md#the-daily-check)) | the clock, and three minutes after the start |

```
sv status /run/service/*
sv restart /run/service/sip
tail -f /var/log/t48/sip/current        # each service's output; three files of 200 KB at most
```

The services are folders with a `run` script in `/etc/service`. **The owner's own go in `/data/local/service`** and win when the name is the same, so any of ours can be replaced, the shell included; an empty file called `down` there switches one of ours off. `t48-services` lays them out in `/run/service` at boot; run it again after adding one.

Order: the shell is up first, so the screen works while the rest starts. baresip starts when there is an account and an address, and saving the account at the screen restarts it (`t48-apply sip`). With no account it waits quietly.

## The watchdog

`t48-watchdog` arms the SoC's watchdog and feeds it every 5 seconds. If nothing feeds it for 30 seconds (the kernel has hung) the phone resets. A crash of the feeder alone does nothing: runit restarts it at once. `sv down /run/service/watchdog` turns the watchdog off.

While the phone is started by kexec from Yealink's firmware, a reset lands back in Yealink's firmware, because that is what the flash boots.

Before that, the loader's own 90-second watchdog covers the boot: `rcS` turns it off once the system image is up (`t48-watchdog off`), and rescue turns it off so that it can stay. `back-to-yealink` resets the phone with `t48-watchdog reset`.

The same program works under both kernels and picks its way when it starts: through `/dev/watchdog` where the kernel has a driver for the timer (Linux 6.18, the standard ARM SP805 driver), and straight at the registers through `/dev/mem` where it has none (the 3.6 fallback). Its first line in `/var/log/t48/watchdog/current` says which.

## Sizes that matter

- The kernel file with the first stage must stay under 4 MB for recovery-mode boots and under 2.5 MB compressed for the kernel partition.
- A recovery-mode ramdisk has 12 to 16 MB before it reaches U-Boot.
