# Other models: the T46S

Issue [#10](https://github.com/M0LTE/yealink-t48s/issues/10). This page is what we know of the SIP-T46S, the T48S's smaller sibling, how the firmware tells the two apart, and what is still to be seen on one.

**Where this stands (2026-11-26): in the standard build and installer; not yet installed on one.** A tester started the live set on a T46S on 25 November and confirmed the picture, every key, the small-screen layout, DND, the network, and calls both ways. Since 2026-11-26 the T46S has the T48S's path: its own stick, `t46s-stick.zip`, with Back up, Install and Restore on the phone's screen, A/B system slots, updates on the stable and testing channels, and its own kernel updates ([guide.md](guide.md)). What has not been seen is a write: nobody has backed up, installed or restored a T46S. So the installer writes only when the phone's own U-Boot names the flash layout the driver knows ([below](#the-flash)), and the first install is the [test plan](#test-plan-for-a-t46s-owner) at the end. Until a `v*` release carries it, the stick is in the [`t46s-latest`](https://github.com/M0LTE/yealink-t48s/releases/tag/t46s-latest) pre-release, rebuilt from `main`.

## What the T46S is

"From code" means read from the per-hardware-id branches of code taken off our T48S: Yealink's `keypad.ko`, its start-up scripts and keymaps, and U-Boot 2.5.3.0-T48S. One firmware file serves all four models (`T46S(T48S,T42S,T41S)-66.x.rom`; its header names id 66 first and lists 65, 67, 68), so those branches are the T46S's own code. The caveat is U-Boot: a T46S carries its own build of it, which we have not seen.

| | T46S | T48S, for comparison | From | How sure |
|---|---|---|---|---|
| Hardware id | 66 | 65 | Yealink's scripts, U-Boot | high |
| SoC | Broadcom BCM11188 family, ARM1176 | the same | same firmware, same kernel, same modules | high |
| RAM | 64 MB | 128 MB | U-Boot puts id 66's frame buffers at 0x03f00000 and 0x03f80000, the top megabyte of 64 MB, as it puts the T48S's at the top of 128 MB. A teardown of a T4xS phone shows U-Boot reporting "DRAM: 60 MiB" | medium to high |
| Flash | 128 MB NAND, the same thirteen partitions | the same | U-Boot's one partition table for every id. Checked on each phone before anything is written ([below](#the-flash)) | medium; not yet seen on a T46S |
| Screen | 4.3 inch, 480x272, 16 bits per pixel, backlit | 7 inch, 800x480, 32 bits | datasheet; U-Boot's screen set-up for id 66 | high |
| Screen mounting | upside down: the picture is turned by 180 degrees in software | upright | Yealink's `TalkLogic.sh` turns Qt's picture for the T46S whatever the phone says; U-Boot turns its own when bit 7 of the fourth hardware version number is set | medium to high |
| Colour order in the 16 bits | red in the top 5 bits (assumed) | n/a | not read; a guess | low |
| Touch | none | resistive, TSC2046 | datasheet; `tsc.ko` refuses id 66 | high |
| Keys | 12 dial keys, 4 arrows, OK, X, volume up and down, Headset, Mute, Message, Hold, Redial, Transfer, Speakerphone, **4 soft keys** under the screen, **10 line keys** beside it | no soft or line keys | datasheet; Yealink's `T46.qmap` | high for which keys exist |
| Key matrix | the same controller and pins, 9 rows by 7 columns, the keys in other places | | `keypad_dev_init` treats 65 and 66 alike; places from `T46.qmap` | confirmed on a phone (25 Nov) |
| Hook switch | GPIO29 | the same | no branch on the id | high |
| Lamps | the red lamp at the top right (Holtek chip), and a **matrix** for the rest: two colours in each of the 10 line keys, and Headset, Mute, Speakerphone | three GPIO lamps and the Holtek one | `led_init`, `led_hw_set` | high that it is a matrix; which bit is which lamp is not known |
| Backlight | the same PWM; enable on **GPIO27**; another brightness curve | enable on GPIO18 | `bklight_init`, `bklight_set` | high |
| Audio | handset, loudspeaker, headset: the same block | | no branch on the id found; not searched for in the audio code | medium |
| Ethernet | two gigabit ports on the SoC's switch, PoE | the same | datasheet | high |
| USB | one USB 2.0 socket | the same | datasheet | high |
| Holtek chip | the same three pins and protocol | | U-Boot asks it which phone this is before it knows | high |
| Recovery mode | hold Speakerphone at power-on; TFTP or USB; files `T46S.rom`, `T46S.bin`, `T46S.rfs` | `T48S.*` | U-Boot names the files after the id (66 gives "T46S"); the key from Yealink's recovery guide | medium to high |
| U-Boot version | not known | 2.5.3.0-T48S | a teardown shows "2.5.0.3-T4XS" on another T4xS | unknown |
| Serial console | not known. Expect an unpopulated 3-pin header like the T48S's CON4: 3.3 V, 115200 8N1 | CON4 | the teardown mentions such a header; no pinout published | low |

Sources outside our own dumps: Yealink's SIP-T46S datasheet (V6, May 2017); Yealink's "Recovery Mode on Yealink IP Phones"; Stefan Gloor's teardown, <https://stefan-gloor.ch/voip-phone-hack>; the FCC filing T2C-T46S, <https://fccid.io/T2C-T46S>, whose internal photos we could not fetch (they would show the chips and the header).

### How the facts were dug out

- **Which keymap is the T46S's.** Yealink's app partition holds six keymaps; file names in that partition are scrambled and were not recovered, so the files were told apart by content. Two have 42 keys. One of them places the digits and Speakerphone exactly where U-Boot's key table for id 70 (the T54S) has them, so the other is `T46.qmap`. This was the weakest link in the key table, and it held: every key was checked on a phone on 25 November (below). The shell's key test ([shell.md](shell.md#the-key-test)) shows every key's row, column and code on the screen, in the table or not, so the table can be put right from photos; the kernel also logs every key it does not know.
- **This U-Boot does not fit a T46S's keys.** Our T48S's U-Boot has key tables for ids 65, 70 and 74 and a default; none has Speakerphone where `T46.qmap` puts it. So a T46S has another build of U-Boot. Its screen set-up and file names for id 66 are in ours all the same (shared source), which is why those are "medium to high" and not "high".
- **The screen.** U-Boot's set-up for ids 66 and 70: 480 wide, 272 high, 16 bits, 960 bytes a row, shown from 0x03f80000; when the hardware version says "upside down" it draws at 0x03f00000 and copies the turned picture across.
- **The backlight.** Yealink's level 1 to 99 becomes a 5-bit PWM number, smaller is brighter: 25 up to level 10, then 26 - (2 x (level - 1)) / 10 up to 70, then 29 - (3 x level - 33) / 10. Our ten steps are its values at 10, 20 ... 99: 25, 23, 21, 19, 17, 15, 13, 9, 6, 3.

### The key table

Code = row x 8 + column + 1, as Yealink's driver numbers the matrix. **Confirmed on hardware, 25 November 2026:** a tester pressed every key in the key test and photographed the screen, and each came out at the code in this table, which is the one in `kernel6/dts/bcm11188-yealink-t46s.dts`.

| Code | Key | Linux key |
|---|---|---|
| 57 49 41 | 1 2 3 | `KEY_1` ... |
| 58 50 42 | 4 5 6 | |
| 59 51 43 | 7 8 9 | |
| 60 61 52 | * 0 # | `KEY_KPASTERISK`, `KEY_0`, `KEY_NUMERIC_POUND` |
| 33 37 34 36 | Up Down Left Right | |
| 35, 44 | OK, X | `KEY_ENTER`, `KEY_BACKSPACE` |
| 53, 45 | Volume down, up | |
| 70, 69 | Headset, Mute | `KEY_PROG1`, `KEY_MUTE` |
| 25 26 27 28 | Message, Hold, Redial, Transfer | as on the T48S |
| 29 | Speakerphone | `KEY_PROG2` |
| 68 67 66 65 | soft keys 1 to 4, left to right (Yealink's F1 to F4) | `KEY_MACRO1` to `KEY_MACRO4` |
| 62 54 46 38 30 | the line keys on the left, top to bottom | `KEY_MACRO11` to `KEY_MACRO15` |
| 63 55 47 39 31 | the line keys on the right, top to bottom | `KEY_MACRO16` to `KEY_MACRO20` |

**To check it on a phone:** hold X (or any key that is not a phone key such as volume or Speakerphone) for 5 seconds at the home screen. The key test opens: press each key and photograph the screen, which shows the key's row, column and code and what the table makes of it. Hold any key for 3 seconds to leave.

### The lamp matrix (not driven yet)

Read from `keypad.ko` and Yealink's kernel (`ledmHw_Init`, `ledmHw_Write`). The SoC's LED matrix block is at 0x80002000, in the same block as the backlight PWM (0x80002040).

- Pins: rows GPIO16 to 21, columns GPIO2 to 9, pin function 5. So GPIO3, 4 and 5, the T48S's three key lamps, are matrix columns here.
- Register 0x00: columns in bits 0 to 3, rows in bits 4 to 6, 0x32 in bits 8 to 15, enable bit 16, bits 17 and 18 set for this id, bit 31 written after every change. 0x04 = 0x1000, 0x08 = 0.
- Data: 0x18, 0x1c, 0x20, two rows each, 12 bits a row at bits 0 and 16. For this id a 0 bit is a lit lamp.
- Yealink's lamp word: bit 0 is the Holtek lamp; the rest go six to a row, two columns along, and its third and fourth groups land on rows 4 and 5 of the hardware.
- Clock: chip control 0x8000004c, which the backlight PWM also uses.

There is no driver for it in this first set, on purpose: the block shares its clock with the backlight, and a first start should not risk the screen for the sake of lamps. On a T46S today only the red lamp at the top right works. The driver is the next step once a T46S has started and its registers can be read.

## What was made to know the model

One system image serves both phones. The kernel is built once per model, with that model's device tree on its end: U-Boot asks for `T46S.bin` or `T48S.bin` by itself, so the right kernel reaches the right phone without anyone choosing.

| Part | What decides | Where |
|---|---|---|
| Model | the device tree: `model`, `compatible` (`yealink,sip-t46s` or `yealink,sip-t48s`, both also `yealink,sip-t4xs`), `yealink,hardware-id` | `kernel6/dts/bcm11188-yealink-t46s.dts`, `...-t48s.dts`, shared `yealink-t4xs.dtsi` |
| Check at start | the Holtek chip's own hardware id is compared with the device tree's; a mismatch is a loud line in the kernel log (`wrong image for this model`). The id and the full version are in `/sys/module/t48s_holtek/parameters/hwid` and `hwversion` | `kernel6/drivers/t48s_holtek.c` |
| Memory, screen | device tree: 59 MB and a 480x272 16-bit frame buffer at 0x03f80000, with `rotation = <180>` | the T46S's `.dts` |
| Keys | `yealink,keymap` in the device tree's `keypad` node replaces the built-in table. A place not in the table is sent as `KEY_UNKNOWN` with its code, for the key test | `t48s_keypad.c`, `shell/keytest.c` |
| Backlight | `yealink,enable-pin` and `yealink,pwm-fields` in the `backlight` node | `t48s_backlight.c` |
| Touch | the driver starts on a T48S only | `t48s_touch.c` (unchanged) |
| Key lamps | `gpio-leds` in the T48S's tree; none in the T46S's | |
| Flash | the same driver and table; nothing is written unless U-Boot's environment names that table ([below](#the-flash)) | `kernel6/drivers/t48s_nand_layout.h` |
| Kernel files | `T46S.bin`, `T46S-kernel.uimg`, named `Linux-6.18.65-t46s` in their header. The driver refuses one model's kernel as page 0 of the other's kernel partition; the installer and the updater check the name too | `kernel6/build.sh` with `MODEL=t46s` |
| Userspace | `t48-model` says `t46s` or `t48s` from the device tree, and the Holtek chip must agree. The installer looks for `T46S.rom` and `install/T46S-kernel.uimg` and keeps its backup in `t46s-backup`; the updater fetches `T46S-kernel.uimg` | `t48-model`, `t48-install`, `t48-update` |
| USB | the same controller, on in the device tree; the driver starts it only when U-Boot has not | `t48s_usb.c` |
| Text console | turned with `fbcon=rotate:2` on the T46S | `kernel6/t46s.config`, the live `.dts` |
| The shell's picture | the shell draws at the screen's own size, 480x272, in a layout for small screens ([shell.md](shell.md#small-screens)). Each row is packed to 16 bits and turned on its way to the panel if the device tree says so. An app gets 800x480 fitted to the screen (each screen pixel the average of the picture's pixels under it) unless its manifest says `"screen": "any"` | `shell/be_fb.c`, `shell/fit.c` |
| No touch | every screen says what the four soft keys do along the bottom edge, each label over its key, and they take the place of the buttons a finger would press: the home screen's are Apps, Calls, Contacts and Settings. The keyboard shows what the keypad types | `shell/main.c` and each screen's file; `shell/tests/small/` |

The T48S's device tree now carries its key table and backlight values too, the same numbers as before; the built-in ones remain for a tree without them. Nothing else about the T48S changed.

**Left for later:**

- The line keys do nothing yet, and their lamps and the three key lamps are dark (above). The home screen keeps room beside them for labels.
- 19 punctuation marks cannot be typed from the keypad (`; ( ) & ' "` and the brackets among them): on the T48S they are on the touch keyboard, on the T46S nowhere yet.
- In a call, X does not hang up: End is soft key 4, or put the handset down.
- Screen shots (`tools/shot.py`, `tools/phone-input.sh shot`) assume the T48S's screen.

## The flash

The driver's table and its writeable ranges were read from one T48S. A T46S carries its own build of U-Boot, which we have not seen, so its layout is not taken on trust. At every start the flash driver reads the first 4 KB of the `env` partition, U-Boot's own environment, which is also where Yealink's kernel gets its partition table (`mtdparts`) and U-Boot its boot command. Nothing at all can be written unless the block's CRC is right and:

- `mtdparts` is exactly the driver's table, all thirteen entries;
- `bootcmd` is `run bootnand`;
- `bootnand` ends with `setenv autostart yes; nboot 0x02680000 0 0x015A0000`: U-Boot starts the image at the start of `kernel`, the place the installer writes.

Otherwise the lock does not open, the `/data` blocks are shut and the `userenv` window refuses: the phone's flash is read-only, the Install page says `Flash: Not recognised` and offers Back up alone, and the boot log has what the phone's environment says instead. On Tom's T48S the environment matches. The rest of an install's checks are the T48S's: a backup of the whole chip, checked three ways including every error-correction byte against our code, so a T46S whose chip or error correction differs gets no further than its backup; no bad block in `kernel` or `app`; the chip's size and page geometry; the Holtek chip's hardware id must be 66 and agree with the kernel's device tree.

## Building

```
buildroot/build.sh WORKDIR                                             # the same system image as for the T48S
ROOTFS=firststage.cpio MODEL=t46s LIVE=1 sh kernel6/build.sh W         # in the kernel6 container: W/T46S.bin
ROOTFS=firststage.cpio MODEL=t46s FLASH=1 sh kernel6/build.sh W        # W/T46S-kernel.uimg, for the kernel partition
tools/mkliveimage.py usb STICK bin=W/T46S.bin system=system.img model=T46S kernel=6.18
mkdir STICK/install; cp W/T46S-kernel.uimg system.img STICK/install/   # and SHA256SUMS over the two
```

CI does the same on every build ([build.yml](../.github/workflows/build.yml)): `t46s-stick.zip` (`T46S.rom`, `T46S.bin`, `T46S.rfs`, `README.txt`, `install/`), `t46s-live.tar.gz` for TFTP, and `T46S-kernel.uimg` beside the T48S's in `system-latest` and in every `v*` release. Builds of `main` also put the stick in the rolling [`t46s-latest`](https://github.com/M0LTE/yealink-t48s/releases/tag/t46s-latest) pre-release.

**Updates.** One system image serves both phones; each takes the kernel named for its own model from the channel. A T46S takes nothing from a release without `T46S-kernel.uimg` (`No release for this phone`): those predate its support. So until the next `v*` release, a T46S on the stable channel finds nothing; Settings, Update, Channel `testing` follows `main`.

## Test plan for a T46S owner

**What this is.** The first backup, install and restore on a T46S, from its own screen. It replaces Yealink's firmware; the backup the installer makes is the way back. Use a phone you could do without, and a 5 V supply of 1.5 A or more.

**What could go wrong.** The layout check and the backup's checks run before anything is written, so the likely failure is a refusal with nothing written. After the install the one step never seen on a T46S is U-Boot starting our kernel from flash. If it does not, the phone stops at the logo; recovery mode (Speakerphone at power-on) is in U-Boot, which is never written, and the stick then starts as before and offers Restore.

**A. Start the new stick, nothing written** (Yealink's firmware still installed)

1. Unpack `t46s-stick.zip` from [`t46s-latest`](https://github.com/M0LTE/yealink-t48s/releases/tag/t46s-latest) onto the stick ([guide.md](guide.md#make-the-stick)). Speakerphone at power-on, 2.
2. Settings, Install. Photograph it. Expected: `Back up`, `Step 1 of 2`. `Flash: Not recognised` means the layout check refused: Back up is still offered and only reads; take it if you like, then stop and send step 3.
3. If you can, Settings, Access, SSH, then send `dmesg | grep -E 't48s_(nand|holtek|usb)'`, `t48-install status` and `t48-live status`. The lines that matter: `layout: U-Boot's environment names this layout` (or `NOT KNOWN` and the three `env` lines after it), `bad blocks`, `self-test: 549 attempts`, `controller as found`, and the hardware id.

**B. Back up, install, use**

4. Back up. About 3 minutes. Copy `t46s-backup` off the stick to a PC and keep it private: it holds the phone's keys and Yealink's settings. Do not send it to anyone.
5. Install. About 3 minutes. `Installed`: take the stick out. Expected: the phone restarts by itself, the Yealink logo, then our home screen within half a minute, with `/data` empty (no account).
6. Settings, About: photograph it (version, memory). Settings, Update: Channel `testing`, Check now, then Install and the restart; the last result should be the new version `installed`.
7. Optional: hold X from power-on until the rescue menu shows, photograph it, choose Start.
8. Optional: push a stick in while it runs: Settings, Apps can see it (the USB socket started from cold).

**C. Back to Yealink**

9. Start from the stick again (Speakerphone, 2). Settings, Install, Restore Yealink firmware. `Restored`: take the stick out. Expected: Yealink's firmware, its settings as they were before step 4.

**Report** on issue #10: the photos, the `dmesg` lines, how long each step took, and the exact text of anything unexpected. Never the backup.

## Questions for a T46S, and what answers them

1. Does its U-Boot environment name the T48S's layout and boot command? (A2, A3: the Install page and the `layout` line.)
2. Any bad blocks in `kernel` or `app`? (A3: `bad blocks`; the backup stops at any.)
3. Does its U-Boot leave the error-correction engine at n=4152 k=4100 t=4, as the T48S's does? (A3: `controller as found`; the backup checks every page either way.)
4. Does the Holtek chip answer with hardware id 66? (A3; without it the installer refuses.)
5. Does its U-Boot start our kernel from flash? (B5.)
6. Is the BOOTMODE flag cleared, so Yealink's settings survive? (A3: `t48-live status`; C9.)
7. Does the USB socket start from cold, after a start from flash? (B8.)
8. Its U-Boot version and the full hardware version string. (Serial; `cat /sys/module/t48s_holtek/parameters/hwversion` over SSH.)
