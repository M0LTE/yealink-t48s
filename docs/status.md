# Status

Last updated 2026-11-24. The owner's guide is [guide.md](guide.md).

## Now

The T48S is a working desk phone on this firmware. All of the below has been done on one phone, the author's.

- **Try it from a USB stick.** Recovery mode starts the firmware from RAM; nothing is written ([live-image.md](live-image.md)). Works over TFTP too.
- **Install and go back, from the phone's own screen.** Settings, Install: Back up, then Install; later, Restore Yealink firmware from the same stick. Proven end to end on 2026-11-24 with a release stick: Restore to Yealink's firmware, then from a clean stick Back up, Install and a restart into the installed firmware ([installer.md](installer.md), issues [#8](https://github.com/M0LTE/yealink-t48s/issues/8), [#33](https://github.com/M0LTE/yealink-t48s/issues/33)).
- **Linux 6.18** started by the phone's own U-Boot, from flash and from a stick, with our drivers for every part of the board: screen, backlight, touch, keys, hook, lamps, Ethernet, audio, flash, watchdog, USB ([kernel6/README.md](../kernel6/README.md)).
- **The phone.** Six SIP accounts, calls on handset and speaker, hold, mute, transfer, a second call, contacts, the call list, do not disturb, voicemail ([phone.md](phone.md)).
- **Apps** from a feed, three themes, the web setup page, SSH that is off until turned on at the screen ([apps.md](apps.md), [design](design/README.md), [web.md](web.md), [access.md](access.md)).
- **Games can flip.** An app draws whole pictures in the screen's own spare frames and they are shown at the screen's 67.8 a second, with no tearing and no copy; keys and touches carry their times ([app-protocol.md](app-protocol.md#flipping)). Measured on the phone; nobody has looked at the glass yet.
- **Updates** over the network into two system slots, with a fallback by itself when an update does not start. Stable and testing channels ([install.md](install.md#updates-and-fallback)).
- **USB** needs a proper power supply and nothing else. The freezes of 2026-11-22 and 23 were Yealink's 0.6 A adapter: the phone draws up to 0.9 A (issue [#58](https://github.com/M0LTE/yealink-t48s/issues/58)).

## Known limits

- **One phone.** Nothing here has run on a second T48S. Other U-Boot versions and other flash chips are untested.
- **Power.** On Yealink's 0.6 A adapter the phone can freeze, and the watchdog restarts it 30 seconds later. Use 5 V at 1.5 A or more. The loudspeaker is limited as a margin for phones still on the small adapter (issue [#57](https://github.com/M0LTE/yealink-t48s/issues/57)).
- **A bad block in the `kernel` or `app` partition** stops the installer: `Bad block in kernel or app. Not supported yet`. A bad block anywhere stops the backup.
- **The speakerphone switches** between the two sides, so only one is heard at a time. The echo canceller that lets both talk is built and off until it has been heard on a real call (issue [#35](https://github.com/M0LTE/yealink-t48s/issues/35), [aec.md](aec.md)).
- **The headset socket** has not been tried with a headset plugged in.
- **The loudspeaker's tuning** is flat: Yealink's equaliser values are not used.
- **One kernel slot.** A system update falls back by itself; a kernel that does not start needs a start from the stick.
- **Releases are not signed.** The updater checks SHA-256 sums fetched over HTTPS.
- **Not yet tried by hand on the phone:** the restart when the stick comes out at the end of Install and Restore, as the page now does it (issue [#61](https://github.com/M0LTE/yealink-t48s/issues/61); the run of 2026-11-24 used the Restart button it replaced); the rescue menu from X held at a real power-on (issue [#32](https://github.com/M0LTE/yealink-t48s/issues/32); it has run on the phone's screen with keys injected); a stick pushed into a phone that was started from flash (issue [#60](https://github.com/M0LTE/yealink-t48s/issues/60)).
- **T46S:** a try-only set over TFTP is built. Nobody has started it on a T46S, and the installer must not be used on one ([models.md](models.md), issue [#10](https://github.com/M0LTE/yealink-t48s/issues/10)).
- **Linux 3.6** is still built as a fallback for one release, and the programs are built against its headers until it goes.

Open work: the [issues](https://github.com/M0LTE/yealink-t48s/issues).

## History

A log, newest first. Each entry was true on its date; where a later one overtook it, the entry says so. From "How we got root" down is the first days, on Yealink's own firmware, kept as the record of how the phone was opened.

- **Linux 6.18 is the phone's kernel** (2026-11-24, issue #55): Tom's hands-on test passed (keys, hook, touch, lamps, backlight, handset and speaker calls), so CI, the release and `tools/ourkernel.sh` now take 6.18 by default: `Image-kexec`, and the live set `t48s-live.tar.gz`. Linux 3.6 is still built as `Image-kexec-linux3.6`, a fallback for one release. The cold start through U-Boot followed the same day (`docs/live-image.md`). Not done: the clean-up when 3.6 goes (`kernel6/README.md`).
- **The loudspeaker has a hard limit, and the ringer is gentler** (2026-11-24, issue #57): a loud ring froze the phone until the watchdog reset it. The audio service now limits everything sent to the loudspeaker's amplifier, the ringer's top volume is that limit, and its notes fade into each other. A 20-second ring at volume 15 passes on both kernels. The freeze turned out to be the phone's whole power budget (screen, lamps, processor, sound), and the cause Yealink's 0.6 A adapter: on a bigger supply it does not happen (issue #58, `docs/audiod.md`).
- **The phone starts as a phone, and keeps its settings** (2026-11-23, issues #17, #29, #30): the kernel image holds a small first stage that starts one read-only system image, or stays up as rescue if there is none; runit keeps the shell, the audio service, baresip and a watchdog feeder running (`docs/boot.md`). The whole stock flash was backed up with its spare bytes to two machines and checked, and a restore shown to give back exactly those bytes (`docs/backup.md`). Then the first writes: the driver now writes one range only, the 60 blocks at the end of `app` that Yealink never used, and `/data` lives there in UBI, so the SIP account, host key and settings survive a restart (`docs/flash.md`). Checked on the phone: services restart and the shell finds them again, the watchdog resets a stalled phone in 30 s, Yealink's firmware still boots and sees its partition as before, and after a round trip through Yealink the phone registered by itself from the account kept in flash. The kernel and system slots in flash followed on 2026-11-24 (`docs/install.md`).
- **Web setup page** (2026-11-23, issue #37): a page served by the phone for a browser on the LAN: accounts, network, time zone, sound, screen, contacts, call history, apps, SSH keys, backup. Off until turned on at the screen; a browser pairs with a code the screen shows. Tested on a PC with the simulator, as the ARM build under qemu, and on the phone with a real browser (2026-11-23: pairing, an account, an app from the feed, a pasted key, a backup, a fixed address, off again; four faults found there and fixed). CI runs its tests and the shell's on every push. See `docs/web.md`.
- **SSH off until the owner turns it on** (2026-11-23, issue #28): a published image has no key and no password in it and starts with SSH off. Settings, Access turns it on after a question that shows the phone's host key, and can open password login for ten minutes, with a password shown on the screen, so `ssh-copy-id` can install a first key; it closes when the key arrives. Our own test builds, made with our keys, always run SSH. Checked on the phone end to end, including off and on again through the serial console. See `docs/access.md`.
- **Backlight, dimming and blanking** (2026-11-23, issue #18): `kernel/drivers/t48s_backlight.c` is a standard backlight device (GPIO18 enable, SoC PWM level, the ten values Yealink uses, confirmed against Yealink's own control on the phone). The shell dims the screen after two minutes untouched and switches it off after thirty; a key, a touch, the handset or a call brings it back, and the press that wakes a dark screen does nothing else. Registers read back right at every step; how it looks still needs eyes (`docs/shell.md`).
- **Flash readable from our kernel, read-only** (2026-11-23, issue #5): `kernel/drivers/t48s_nand.c` reads the NAND and cannot write it (no write code, a whitelist of chip commands, read-only partitions; no write tools in the image). All 12 partitions were dumped with their spare bytes under Yealink and under ours: 11 identical byte for byte, and `cfg` differs only because Yealink keeps writing it. The error correction is the phone's own setting (4 bits per 512 bytes, not mainline's 8) and is reproduced in software by `tools/nandcheck.py`. Nothing was written. What writing will need is in `docs/nand.md`.
- **Lamps in our kernel** (2026-11-23, issue #3): the phone has four lamps: message (top right of the case, on the Holtek chip; called power until it was renamed the same day) and one each in the Headset, Mute and Speakerphone keys (GPIO3, 4, 5). `kernel/drivers/t48s_leds.c` shows them as ordinary Linux LEDs under `/sys/class/leds`, with blinking, and the shell drives them. Wiring taken from Yealink's driver and its own lamp table, and checked on the phone by register read-back under both kernels. Nobody has looked at the lamps yet.
- **One image, built by Buildroot** (2026-11-23, issue #16): the phone's root filesystem now comes only from `buildroot/` (Buildroot 2026.02.3, the long-term line); the hand-made one is gone. It carries the shell and the clock app, libt48 (also in the SDK), our test tools, t48rec and t48mix, and baresip and t48-audiod (installed, not started). Booted on the phone: the shell on screen, launcher and clock work, SSH, DHCP and NTP, `tools/arp-test.sh` passes, t48rec records at 16 kHz, baresip starts. Root filesystem 6.8 MB (3.4 MB gzipped), kernel image with it 9.6 MB.
- **Phone extras and several accounts, in the simulator** (2026-11-23, issues #27 and #36): contacts, do not disturb, call waiting and swapping between two calls, asking first before a transfer, the message lamp for voicemail, and up to six SIP accounts with a choice of which one to call from. Tested with scripted calls against the real baresip and audio service on a PC; not yet run on the phone. See `docs/phone.md`.
- **The shell is a phone, in the simulator** (2026-11-23, issue #26): dialler, call screens, history, and the hook, Speakerphone, Headset, Mute, Hold, Transfer, Redial and Volume keys doing what they do on a desk phone. Calls take the screen from apps and give it back. Tested with scripted calls against the real baresip and audio service on a PC; not yet run on the phone, and sound still waits on the audio driver. See `docs/phone.md`.
- **The shell runs on the phone** (2026-11-23, issue #19): our kernel now boots straight to `t48-shell` on the screen, with the clock app installed. Idle screen, launcher, the app, Home, the handset placeholder and the clean-up of a killed app were all checked on the real phone by injecting keys and touches from a PC (`tools/phone-input.sh`); nobody has pressed a real key on it yet. The short list for fingers is in `docs/shell.md`.
- **ARP fixed in our kernel** (2026-11-23, issue #34): the switch's CPU port had broadcasts turned off after every boot of our kernel, so the phone never heard ARP requests and vanished for any host that had forgotten its MAC address. The driver now sets and checks that register. `tools/arp-test.sh` shows it: fails on the old image, passes on the new one.
- **Touchscreen in our kernel** (2026-11-22, issue #4): `kernel/drivers/t48s_touch.c`, a Linux input device with raw 12-bit positions like Yealink's, polled. Tom tapped the corners and dragged: dots land under the finger using the factory calibration (U-Boot's `calparam`, now `/etc/pointercal` in our root filesystem). Test again any time with `tools/ourkernel.sh ssh touchtest`.
- **Keys, hook switch and the keypad chip's watchdog in our kernel** (2026-11-22, issue #3): `kernel/drivers/t48s_keypad.c` (keys and hook as Linux input devices, polled) and `kernel/drivers/t48s_holtek.c` (reads the chip's version, turns its watchdog off at boot). The drivers load, read "no key pressed", and the watchdog is proven both ways. **No key has been pressed yet**: see the test steps in `kernel/README.md`. Six function keys still need their labels matched.
- **Screen console in our kernel** (2026-11-22, issue #2): `kernel/drivers/t48s_fb.c` makes the screen U-Boot leaves running into `/dev/fb0`; kernel messages, status and a login prompt show on it (`docs/img/fbcon-2026-11-22.png`). The loader now stops all DMA before jumping: Yealink's audio DMA kept writing into RAM our kernel uses, which crashed one boot and probably caused the earlier network trouble.
- **Ethernet in our kernel** (2026-11-22, issue #1): our own driver (`kernel/drivers/t48s_eth.c`) for the on-chip switch. Our kernel gets its address by DHCP and runs SSH (dropbear, key login): `tools/ourkernel.sh ssh`. A 4 MB copy over SSH takes 1.5 s and arrives intact.
- **Hands-off kernel iteration** (2026-11-22): `tools/kexec-boot.sh` starts our kernel from Yealink's running system (module `kmod/t48kexec.ko`), its shell is driven over serial (`sercmd.py` on proxmox0), and `back-to-yealink` returns to Yealink with SSH. No key presses, no flash writes. See `kmod/README.md`.
- Rescue route if SSH is ever lost: `tools/mkrescue.sh` + recovery mode option 1 (Speakerphone at power-on).

- **Our own kernel runs** (2026-11-22): Linux 3.6.11 for the board, network-booted via recovery mode, busybox shell on the serial console, banner on the screen. See `kernel/README.md`.

- **Network boot of our own userspace** (2026-11-22): recovery mode (Speakerphone at power-on, menu option 1, TFTP from `tools/tftpd.py`) boots `T48S.bin` and our `T48S.rfs`. Serial console works both ways; no watchdog resets. USB recovery hung with both sticks tried (2026-11-22 and 2026-11-23), before U-Boot read anything from them; the cause was the power supply, found 2026-11-24: [live-image.md](live-image.md#why-the-sticks-froze). Details in own-kernel.md.

- Touch test passed (2026-11-22): taps land on the buttons, keys and the handset drop back to Yealink. Linux keycodes under Yealink are keypad matrix positions (digits 1-3 = 65 66 67, 4-6 = 57 58 59, 7-9 = 49 50 51, * 0 # = 41 42 43, hook = 89; from Yealink's own keymap, see keypad-touch-hw.md. An earlier version of this note was one row off); U-Boot's scan codes use different numbers.

- Root shell on the phone, over the network, no hardware needed.
- Our own compiled program running on the phone and drawing the whole screen.
- Serial console wired up for watching boots.
- SSH: static dropbear (musl, built with `native/build-dropbear.sh`) installed in `/data/ssh`, key login as root. `scp` needs `-O` (no SFTP server).
- Boot hook: Yealink's `phone.sh` runs `/config/simulator.sh` *instead of* its normal startup when that file exists. Ours starts SSH, then hands over to Yealink's original startup. SSH now survives reboots, and the phone otherwise behaves as stock.
- **Our own screen alongside Yealink's** (`t48ui`), starting at boot. It shows GB7RDG's open links, sessions and neighbours, read from LinBPQ's web server every 15 s. The "GB7RDG" line key (key 7) on Yealink's screen opens it; the Phone button, any physical key, or any call goes back to Yealink. See "Screen switching" below.

## How we got root

The phone came with firmware 66.86.0.15. Its web UI's ping diagnostics box used to run its input through a shell, but that is fixed in 66.84 and later, and U-Boot ignores serial input.

The web UI accepts older firmware, so we downgraded: 66.86.0.15 to 66.84.0.125 (still fixed) to 66.82.0.31. On 66.82 the web UI is plain HTTP and the ping box runs commands as root:

```
ip = 127.0.0.1 -c 1;<command>
```

From there `/usr/sbin/telnetd -p 2323 -l /bin/sh` gives a root shell.

Firmware sources (not in this repo):

| Version | Where | sha256 |
|---|---|---|
| 66.86.0.15 (shipped) | download.1afa.com `.../yealink/firmware/T4xS/` | `47f1c8fc...a1a56` |
| 66.84.0.125 | same | `6a114528...da560` |
| 66.82.0.31 (current) | github.com/0E800/Yealink (split zip) | `dbc92f74...919c6` |

## The hardware

From the U-Boot banner and the running system:

- Broadcom BCM11188, ARMv6 (`armv6l`), 124 MB RAM
- 128 MB Winbond NAND, 2 KB pages, 128 KB erase blocks
- U-Boot 2.5.3.0-T48S, Linux 2.6.27.47, busybox 1.10.3 (very minimal)
- Holtek companion MCU (involved in firmware key derivation)
- Screen: `/dev/fb0`, 800x480, 32 bits per pixel (BGRX), 3200 bytes per line
- Input: `/dev/input/TouchScreen`, `/dev/keypad0`, `/dev/input/event2`

Flash partitions (`/proc/mtd`):

| mtd | Offset | Size | Name | Use |
|---|---|---|---|---|
| 0 | 0x0000000 | 128 KB | ubl | first-stage loader |
| 1 | 0x0020000 | 1 MB | uboot | U-Boot |
| 2 | 0x0120000 | 1 MB | env | U-Boot environment |
| 3 | 0x0220000 | 1 MB | userenv | |
| 4 | 0x0320000 | 1 MB | logo | boot splash |
| 5 | 0x0420000 | 2.5 MB | smkern | safe-mode kernel |
| 6 | 0x06a0000 | 15 MB | smrfs | safe-mode root fs |
| 7 | 0x15a0000 | 2.5 MB | kernel | Linux kernel |
| 8 | 0x1820000 | 15 MB | rfs | root fs, yaffs2, mounted read-only |
| 10 | 0x2720000 | 75 MB | app | `/phone` and `/boot`, yaffs2, read-only |
| 11 | 0x7200000 | 4 MB | cfg | `/config`, yaffs2, writable |
| 12 | 0x7600000 | 10 MB | data | `/data`, yaffs2, writable |

## How Yealink's software runs

- `/phone/scripts/phoneapp.sh` runs the UI `/phone/bin/t48/dskPhone.exx -qws` (Qt Embedded) in an endless loop.
- SIP is a separate process, `sipServer.exx`; the web UI is lighttpd plus `fcgiServer.exx`.
- Pausing `dskPhone.exx` with SIGSTOP makes the phone reboot within a few minutes, so something supervises it. Replacing the UI means changing how it is launched, not just pausing it.

## SSH and the boot hook

Files on the phone:

| Path | What |
|---|---|
| `/data/ssh/dropbearmulti` | dropbear, dropbearkey, scp, dbclient (symlinks alongside) |
| `/data/ssh/dropbear_ed25519_host_key` | host key, fingerprint `SHA256:lIPt4HWHPqX6WiCIdtriaOeP9snbZc8m11q+4YmkdBk` |
| `/data/ssh/authorized_keys` | keys allowed to log in as root (dropbear `-D /data/ssh`, since root's home `/` is read-only) |
| `/data/custom/start.sh` | our services, started at boot (from `phone/start.sh`): dropbear and t48ui |
| `/data/custom/t48ui`, `fbswap.so`, `tap`, `envexec` | built from `native/` |
| `/data/custom/rearm.sh` | puts the boot hook back after 2 minutes if SSH is up (from `phone/rearm.sh`) |
| `/config/simulator.sh` | the boot hook (from `phone/simulator.sh`); `.armed` suffix while disarmed |

Start SSH by hand: `/data/ssh/dropbear -r /data/ssh/dropbear_ed25519_host_key -D /data/ssh -s -p 22 -P /var/dropbear.pid`

Other boot facts: `/etc/inittab` runs a getty on the serial port, but it gave no response to Enter. `/phone/scripts/TelnetServices.sh` starts Yealink's own telnetd when the config setting `features.temode` is 1. There is also a uid-0 account `toor` in `/etc/passwd`.

## Screen switching

The display driver refuses any change to the screen layout, so there is no page flipping. Instead:

- `fbswap.so` is loaded into every process via `LD_PRELOAD`, set by the boot hook. It does nothing except in Yealink's UI (`dskPhone.exx`), where it records the address of its `/dev/fb0` mapping and writes its PID to `/tmp/fbswap.pid`.
- On signal 44 it reads `/tmp/fbswap.mode`. `o`: the first 1.5 MB of that mapping (the visible screen) is replaced, at the same address, by the tmpfs file `/tmp/fbswap.shadow`, so Yealink keeps drawing unseen. `y`: the real framebuffer is mapped back and the shadow is copied onto the screen. No copying at all while idle.
- `t48ui` (static, musl) does the switching: fbswap mode, an exclusive grab (`EVIOCGRAB`) of the keypad (event0, node made in `/tmp`), expansion port (event1) and touchscreen (event2), and the backlight via `/proc/keypad/lcd_bklight` (lit while shown, dims after 10 minutes, restored on the way out). Physical keys hide our screen and are written back into the device so Yealink sees them.
- Control on port 8123: `/show`, `/hide`, `/toggle`, `/status`. Yealink calls these itself: line key 7 is a URL key for `/show` (`tools/linekey.py`), and Action URLs for incoming call, off-hook, call out and idle-to-busy call `/hide`; setup-completed calls `/status` (`tools/actionurls.py`).
- Touch positions come from the tslib calibration in `/etc/pointercal` (12-bit raw values). `/data/custom/tap X Y` injects a tap for testing; injected taps do not reach a grabbed device.

Why freezing Yealink's UI rebooted the phone: the keypad driver has a watchdog (`/proc/keypad/watchdog`) that the UI feeds.

## Building for the phone

`native/` uses clang with no C library at all (raw system calls), which sidesteps the phone's 2016-era glibc:

```sh
cd native && make
python3 ../tools/push.py fbtest /tmp/fbtest
python3 ../tools/tsh.py 'chmod +x /tmp/fbtest; /tmp/fbtest &'
```

`fbtest` draws a test card and logs touch and key events. `make` builds everything; the musl-linked ones need the toolchain from `build-dropbear.sh`, and `gen/fonts.h` is generated from DejaVu fonts by `tools/mkfont.py`.

## Serial console

CON4 is a 3-pin UART, 3.3 V, 115200 8N1: green GND, orange is the board's TX, purple is the board's RX. Connected to an FTDI adapter on proxmox0 (`/dev/ttyUSB0`). U-Boot prints its banner but ignores input; Linux keeps the console quiet.

Holding a keypad button at power-on shows up as `scan key=N` (OK=16, X=17, \*=10, #=11). OK is polled repeatedly, so a long press of OK probably starts something (recovery or factory reset). Not tested yet.

## Prior art

- Stefan Gloor, "Hacking a VoIP Phone" (stefan-gloor.ch/voip-phone-hack): same T4XS family, flash layout, root by patching the updater.
- Synacktiv, "No grave but the SIP" and github.com/synacktiv/yealink_tools: firmware format; their decryption covers only older images.
