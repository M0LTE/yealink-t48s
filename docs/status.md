# Status

Last updated 2026-11-22.

## Done

- Root shell on the phone, over the network, no hardware needed.
- Our own compiled program running on the phone and drawing the whole screen.
- Serial console wired up for watching boots.
- SSH: static dropbear (musl, built with `native/build-dropbear.sh`) installed in `/data/ssh`, key login as root. `scp` needs `-O` (no SFTP server).
- Boot hook: Yealink's `phone.sh` runs `/config/simulator.sh` *instead of* its normal startup when that file exists. Ours starts SSH, then hands over to Yealink's original startup. SSH now survives reboots, and the phone otherwise behaves as stock.

## Now

- **The phone's address moves.** It has been 10.45.0.217 and, since a reboot, 10.45.0.214 (DHCP). Its MAC is `80:5e:c0:03:1c:05`; find it with a ping sweep and `ip neigh`. A DHCP reservation on the router would fix this. `tools/phonecfg.py` defaults to .214; override with `T48S_HOST`.

## Next

1. **Our own UI alongside Yealink's.** A launcher that can switch the screen between Yealink's phone UI and ours, so the SIP phone keeps working.
2. **Packet-radio display.** For example the connections currently open on GB7RDG. Needs a small feed from the node side.
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
| `/data/custom/start.sh` | our services, started at boot (from `phone/start.sh`) |
| `/data/custom/rearm.sh` | puts the boot hook back after 2 minutes if SSH is up (from `phone/rearm.sh`) |
| `/config/simulator.sh` | the boot hook (from `phone/simulator.sh`); `.armed` suffix while disarmed |

Start SSH by hand: `/data/ssh/dropbear -r /data/ssh/dropbear_ed25519_host_key -D /data/ssh -s -p 22 -P /var/dropbear.pid`

Other boot facts: `/etc/inittab` runs a getty on the serial port, but it gave no response to Enter. `/phone/scripts/TelnetServices.sh` starts Yealink's own telnetd when the config setting `features.temode` is 1. There is also a uid-0 account `toor` in `/etc/passwd`.

## Building for the phone

`native/` uses clang with no C library at all (raw system calls), which sidesteps the phone's 2016-era glibc:

```sh
cd native && make
python3 ../tools/push.py fbtest /tmp/fbtest
python3 ../tools/tsh.py 'chmod +x /tmp/fbtest; /tmp/fbtest &'
```

`fbtest` draws a test card and logs touch and key events.

## Serial console

CON4 is a 3-pin UART, 3.3 V, 115200 8N1: green GND, orange is the board's TX, purple is the board's RX. Connected to an FTDI adapter on proxmox0 (`/dev/ttyUSB0`). U-Boot prints its banner but ignores input; Linux keeps the console quiet.

Holding a keypad button at power-on shows up as `scan key=N` (OK=16, X=17, \*=10, #=11). OK is polled repeatedly, so a long press of OK probably starts something (recovery or factory reset). Not tested yet.

## Prior art

- Stefan Gloor, "Hacking a VoIP Phone" (stefan-gloor.ch/voip-phone-hack): same T4XS family, flash layout, root by patching the updater.
- Synacktiv, "No grave but the SIP" and github.com/synacktiv/yealink_tools: firmware format; their decryption covers only older images.
