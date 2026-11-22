# Status

Last updated 2026-11-22.

## Done

- **Our own kernel runs** (2026-11-22): Linux 3.6.11 for the board, network-booted via recovery mode, busybox shell on the serial console, banner on the screen. See `kernel/README.md`.

- **Network boot of our own userspace** (2026-11-22): recovery mode (Speakerphone at power-on, menu option 1, TFTP from `tools/tftpd.py`) boots `T48S.bin` and our `T48S.rfs`. Serial console works both ways; no watchdog resets. USB recovery hangs with our stick. Details in own-kernel.md.

- Touch test passed (2026-11-22): taps land on the buttons, keys and the handset drop back to Yealink. Linux keycodes are keypad matrix positions (digits 1-9 = 57 58 59 / 49 50 51 / 41 42 43, hook = 89); U-Boot's scan codes use different numbers.

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
2. **Our own kernel: drivers.** It boots to a shell (kernel/README.md). Next: a simple framebuffer driver over U-Boot's screen memory (console on the screen), then Ethernet (the on-chip switch; mainline has no driver), keypad and touch.
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
