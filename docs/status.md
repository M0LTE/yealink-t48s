# Status

Last updated 2026-11-23.

## Done

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

- **Network boot of our own userspace** (2026-11-22): recovery mode (Speakerphone at power-on, menu option 1, TFTP from `tools/tftpd.py`) boots `T48S.bin` and our `T48S.rfs`. Serial console works both ways; no watchdog resets. USB recovery hangs with our stick. Details in own-kernel.md.

- Touch test passed (2026-11-22): taps land on the buttons, keys and the handset drop back to Yealink. Linux keycodes under Yealink are keypad matrix positions (digits 1-3 = 65 66 67, 4-6 = 57 58 59, 7-9 = 49 50 51, * 0 # = 41 42 43, hook = 89; from Yealink's own keymap, see keypad-touch-hw.md. An earlier version of this note was one row off); U-Boot's scan codes use different numbers.

- Root shell on the phone, over the network, no hardware needed.
- Our own compiled program running on the phone and drawing the whole screen.
- Serial console wired up for watching boots.
- SSH: static dropbear (musl, built with `native/build-dropbear.sh`) installed in `/data/ssh`, key login as root. `scp` needs `-O` (no SFTP server).
- Boot hook: Yealink's `phone.sh` runs `/config/simulator.sh` *instead of* its normal startup when that file exists. Ours starts SSH, then hands over to Yealink's original startup. SSH now survives reboots, and the phone otherwise behaves as stock.
- **Our own screen alongside Yealink's** (`t48ui`), starting at boot. It shows GB7RDG's open links, sessions and neighbours, read from LinBPQ's web server every 15 s. The "GB7RDG" line key (key 7) on Yealink's screen opens it; the Phone button, any physical key, or any call goes back to Yealink. See "Screen switching" below.

## Needs Tom

(Phone address is now fixed at 10.45.0.214 by a DHCP reservation, `yealink-t48s` on the router, 2026-11-22. Tom's RSA key is in `/data/ssh/authorized_keys`.)


## Next

1. **More pages and the keypad:** map the keys once Tom has pressed them; more packet views (heard lists, per-port traffic); maybe a page switcher.
2. **Our own kernel: drivers.** It boots to a shell (kernel/README.md). Ethernet and SSH work. The screen console works too. Keys and hook have drivers (#3), and so does touch (#4).
3. **Our own SIP phone.** Later, and only if the audio path can be driven without Yealink's software.

## How we got root

The phone came with firmware 66.86.0.15. Its web UI's ping diagnostics box used to run its input through a shell, but that is fixed in 66.84 and later, and U-Boot ignores serial input.

The web UI accepts older firmware, so we downgraded: 66.86.0.15 to 66.84.0.125 (still fixed) to 66.82.0.31. On 66.82 the web UI is plain HTTP and the ping box runs commands as root:

```
ip = 127.0.0.1 -c 1;<command>
```

From there `/usr/sbin/telnetd -p 2323 -l /bin/sh` gives a root shell. It does not survive a reboot yet.

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
