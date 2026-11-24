# For developers

Where to read, by what you want to change. The owner's guide is [guide.md](guide.md); where things stand is [status.md](status.md); the design is [architecture.md](architecture.md).

| | |
|---|---|
| How the phone starts, the two files of a boot, services, rescue | [boot.md](boot.md) |
| The kernel (Linux 6.18) and its drivers | [kernel6/README.md](../kernel6/README.md) |
| The root filesystem (Buildroot) | [buildroot/README.md](../buildroot/README.md) |
| Starting from a stick or TFTP, U-Boot's checks, the BOOTMODE flag | [live-image.md](live-image.md) |
| The on-phone installer, USB, its test plan | [installer.md](installer.md) |
| What an install writes, slots, updates, fallback | [install.md](install.md) |
| The flash: layout, what the driver will and will not write | [flash.md](flash.md), [nand.md](nand.md) |
| Backing up from a PC, and the way back | [backup.md](backup.md) |
| The screen: shell, launcher, apps, the simulator | [shell.md](shell.md), [app-protocol.md](app-protocol.md) |
| The phone: calls, accounts, contacts | [phone.md](phone.md), [sip.md](sip.md) |
| Sound | [audio.md](audio.md), [audiod.md](audiod.md), [aec.md](aec.md) |
| Settings and the web setup page | [settings.md](settings.md), [web.md](web.md) |
| SSH and who may log in | [access.md](access.md) |
| Apps: the manager, the feed, the SDK | [apps.md](apps.md), [sdk/README.md](../sdk/README.md) |
| Words and look | [style.md](style.md), [design/](design/README.md) |
| Keys, touch, the board | [keypad-touch-hw.md](keypad-touch-hw.md) |
| How we got in, before any of this | [own-kernel.md](own-kernel.md) |

## Build

CI ([`.github/workflows/build.yml`](../.github/workflows/build.yml)) is the recipe: the root filesystem, the kernels, the stick. By hand:

```
buildroot/build.sh WORK                                        # WORK/output/images: firststage.cpio, system.img
ROOTFS=firststage.cpio LIVE=1  sh kernel6/build.sh K           # in the kernel6 container: K/T48S.bin
ROOTFS=firststage.cpio FLASH=1 sh kernel6/build.sh K           # K/T48S-kernel.uimg
tools/mkliveimage.py usb STICK bin=K/T48S.bin system=system.img kernel=6.18
mkdir STICK/install && cp K/T48S-kernel.uimg system.img STICK/install/
(cd STICK/install && sha256sum T48S-kernel.uimg system.img > SHA256SUMS)
sudo tools/mkusbstick.sh /dev/sdX STICK
```

## Test without a phone

```
make -C shell check          # the screens, in the simulator
make -C webconfig check      # the web setup page
tools/test-owner.sh bytes SET BACKUP      # the on-phone installer on a flash in a file
tools/test-install.sh bytes SET BACKUP    # the PC installer, the same way
sudo tools/test-owner.sh ubi SET          # both app steps on Linux's simulated flash chip
sudo tools/test-install.sh ubi SET
tools/test-userenv.sh; tools/test-bootmode.sh; tools/test-live.sh
```

## Tools

`tools/` holds Python and shell helpers run from a Linux box on the phone's network.

- A development build carries SSH keys in the kernel: `ssh root@<phone>`, files with `scp -O` ([access.md](access.md#our-own-test-builds)).
- `t48install.sh`, `nandbackup.sh`, `nandrestore.sh`: install, back up and restore from a PC over SSH.
- `mkliveimage.py`, `mkusbstick.sh`, `tftpd.py`: the stick and the TFTP set.
- `shot.py`, `phone-input.sh`: a screenshot of the phone, and keys and touches sent to it.
- `ylweb.py`, `ylweb82.py`, `diag82.py`, `tsh.py`, `push.py`: Yealink's own web UI and the root shell through firmware 66.82's ping box, from before the phone ran our kernel. Firmware 66.84 and later close that hole.

Kernel code is GPL-2.0; everything else is AGPL-3.0-or-later. New files carry an `SPDX-License-Identifier` line. Third-party code and the release's source archive: [NOTICE.md](../NOTICE.md). The first release's notes: [release-notes-draft.md](release-notes-draft.md).
