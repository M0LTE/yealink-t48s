# Other models: the T46S

Issue [#10](https://github.com/M0LTE/yealink-t48s/issues/10). The T48S is the only phone this firmware has run on. This page is what we know of its sibling the SIP-T46S, how the firmware was made to tell the two apart, and how an owner can try it on a T46S without writing anything to the phone.

**Where this stands (2026-11-25): try-only; it starts.** Nobody here has a T46S; a tester started the live set on one on 25 November: U-Boot took it from a USB stick, the picture came up the right way round in the right colours, and DHCP worked. Keys, sound and the rest are still to be tried. Everything below was worked out from Yealink's and U-Boot's code, which is shared between the models and branches on a hardware id, and from Yealink's datasheet. A T46S live set is built with every build of `main` and passes the same file checks as the T48S one: download `t46s-live.zip` from the [`t46s-latest`](https://github.com/M0LTE/yealink-t48s/releases/tag/t46s-latest) pre-release. The test plan is at the end. The installer and the release's `t48s-stick.zip` are for the T48S only.

## What the T46S is

"From code" means read from the per-hardware-id branches of code taken off our T48S: Yealink's `keypad.ko`, its start-up scripts and keymaps, and U-Boot 2.5.3.0-T48S. One firmware file serves all four models (`T46S(T48S,T42S,T41S)-66.x.rom`; its header names id 66 first and lists 65, 67, 68), so those branches are the T46S's own code. The caveat is U-Boot: a T46S carries its own build of it, which we have not seen.

| | T46S | T48S, for comparison | From | How sure |
|---|---|---|---|---|
| Hardware id | 66 | 65 | Yealink's scripts, U-Boot | high |
| SoC | Broadcom BCM11188 family, ARM1176 | the same | same firmware, same kernel, same modules | high |
| RAM | 64 MB | 128 MB | U-Boot puts id 66's frame buffers at 0x03f00000 and 0x03f80000, the top megabyte of 64 MB, as it puts the T48S's at the top of 128 MB. A teardown of a T4xS phone shows U-Boot reporting "DRAM: 60 MiB" | medium to high |
| Flash | 128 MB NAND, the same thirteen partitions | the same | U-Boot's one partition table for every id | medium |
| Screen | 4.3 inch, 480x272, 16 bits per pixel, backlit | 7 inch, 800x480, 32 bits | datasheet; U-Boot's screen set-up for id 66 | high |
| Screen mounting | upside down: the picture is turned by 180 degrees in software | upright | Yealink's `TalkLogic.sh` turns Qt's picture for the T46S whatever the phone says; U-Boot turns its own when bit 7 of the fourth hardware version number is set | medium to high |
| Colour order in the 16 bits | red in the top 5 bits (assumed) | n/a | not read; a guess | low |
| Touch | none | resistive, TSC2046 | datasheet; `tsc.ko` refuses id 66 | high |
| Keys | 12 dial keys, 4 arrows, OK, X, volume up and down, Headset, Mute, Message, Hold, Redial, Transfer, Speakerphone, **4 soft keys** under the screen, **10 line keys** beside it | no soft or line keys | datasheet; Yealink's `T46.qmap` | high for which keys exist |
| Key matrix | the same controller and pins, 9 rows by 7 columns, the keys in other places | | `keypad_dev_init` treats 65 and 66 alike; places from `T46.qmap` | medium: see below |
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

- **Which keymap is the T46S's.** Yealink's app partition holds six keymaps; file names in that partition are scrambled and were not recovered, so the files were told apart by content. Two have 42 keys. One of them places the digits and Speakerphone exactly where U-Boot's key table for id 70 (the T54S) has them, so the other is `T46.qmap`. This is the weakest link in the key table: if it is wrong, keys do the wrong thing or nothing, and the kernel logs every key it does not know with its row and column.
- **This U-Boot does not fit a T46S's keys.** Our T48S's U-Boot has key tables for ids 65, 70 and 74 and a default; none has Speakerphone where `T46.qmap` puts it. So a T46S has another build of U-Boot. Its screen set-up and file names for id 66 are in ours all the same (shared source), which is why those are "medium to high" and not "high".
- **The screen.** U-Boot's set-up for ids 66 and 70: 480 wide, 272 high, 16 bits, 960 bytes a row, shown from 0x03f80000; when the hardware version says "upside down" it draws at 0x03f00000 and copies the turned picture across.
- **The backlight.** Yealink's level 1 to 99 becomes a 5-bit PWM number, smaller is brighter: 25 up to level 10, then 26 - (2 x (level - 1)) / 10 up to 70, then 29 - (3 x level - 33) / 10. Our ten steps are its values at 10, 20 ... 99: 25, 23, 21, 19, 17, 15, 13, 9, 6, 3.

### The key table

Code = row x 8 + column + 1, as Yealink's driver numbers the matrix.

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
| 68 67 66 65 | soft keys 1 to 4 (Yealink's F1 to F4) | `KEY_MACRO1` to `KEY_MACRO4` |
| 62 54 46 38 30 | line keys 1 to 5 | `KEY_MACRO11` to `KEY_MACRO15` |
| 63 55 47 39 31 | line keys 6 to 10 | `KEY_MACRO16` to `KEY_MACRO20` |

Which soft key is leftmost and which line key is at the top are guesses.

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
| Keys | `yealink,keymap` in the device tree's `keypad` node replaces the built-in table | `t48s_keypad.c` |
| Backlight | `yealink,enable-pin` and `yealink,pwm-fields` in the `backlight` node | `t48s_backlight.c` |
| Touch | the driver starts on a T48S only | `t48s_touch.c` (unchanged) |
| Key lamps | `gpio-leds` in the T48S's tree; none in the T46S's | |
| Flash | the T46S kernel has no flash driver at all, and its device tree no flash | `kernel6/build.sh` with `MODEL=t46s` |
| Text console | turned with `fbcon=rotate:2` on the T46S | `kernel6/t46s.config`, the live `.dts` |
| The shell's picture | the shell still draws 800x480. On a screen of another size or depth, or one mounted upside down, each screen pixel is the average of the picture's pixels under it, turned if the device tree says so | `shell/be_fb.c` |
| No touch | every screen was checked for things only a finger could do. The four buttons along the bottom of Calls and Contacts were the gap; the T46S's four soft keys now press them, left to right | `shell/main.c`, `phone.c`, `contacts.c`; `shell/tests/11-softkeys.script` |

The T48S's device tree now carries its key table and backlight values too, the same numbers as before; the built-in ones remain for a tree without them. Nothing else about the T48S changed.

**Left for later**, none of it in the way of a first start:

- A layout drawn for 480x272. The fitted picture is readable (text comes out at about 60% of its T48S size) but small.
- The line keys do nothing yet, and their lamps and the three key lamps are dark (above).
- Three things still need a finger on the T48S and have no key on either phone: 19 punctuation marks on the keyboard (`; ( ) & ' "` and the brackets among them), "Close now" on the password notice (the Access page has a row for it), and showing the list of tones sent in a call (the tones themselves work from the keys).
- In a call, X does not hang up: put the handset down, or press Speakerphone again.
- Screen shots (`tools/shot.py`, `tools/phone-input.sh shot`) assume the T48S's screen.

## Building the T46S live set

```
buildroot/build.sh WORKDIR                                             # the same system image as for the T48S
ROOTFS=firststage.cpio MODEL=t46s LIVE=1 sh kernel6/build.sh W         # in the kernel6 container: W/T46S.bin
tools/mkliveimage.py build OUT bin=W/T46S.bin system=system.img model=T46S kernel=6.18
```

`MODEL=t46s` refuses anything but a live build. The set is `T46S.rom` (our 4 KB label, which already listed id 66), `T46S.bin` and `T46S.rfs`. CI builds it the same way on every build of `main` and publishes it as `t46s-live.zip` in the rolling pre-release [`t46s-latest`](https://github.com/M0LTE/yealink-t48s/releases/tag/t46s-latest): the three files, `SHA256SUMS` and a `README.txt` with the owner's steps (`tools/t46s-live-readme.txt`). A `v*` release carries the same zip.

## Test plan for a T46S owner

**What this is.** A start of the open firmware from RAM, through the phone's own recovery mode, over the network or from a USB stick. Nothing of ours is written to the phone: this kernel has no flash driver. Unplugging the power ends it.

**What it costs.** One thing, and it is Yealink's doing, not ours: after a recovery start over the network, Yealink's firmware resets its own settings (account, passwords, language) the next time it starts. Use a phone whose settings you can lose, or export them first (Yealink's web page: Settings, Configuration, Export). The phone itself is not at risk: recovery mode lives in U-Boot, which nothing here touches.

Do not use the T48S's stick or its Install page on a T46S: the kernel it writes is the T48S's. This page's try-only start comes first.

**You need**

- The three files `T46S.rom`, `T46S.bin`, `T46S.rfs` in one folder: unpack `t46s-live.zip` from the [`t46s-latest`](https://github.com/M0LTE/yealink-t48s/releases/tag/t46s-latest) pre-release.
- A PC on the same network as the phone, with a TFTP server serving that folder. On Linux or macOS: `sudo python3 tools/tftpd.py FOLDER 69 --rom-once` from this repository (it prints every file the phone asks for, which we want to see). On Windows, Tftpd64 pointed at the folder.
- The PC's address, a free address for the phone, the netmask and the gateway.
- A camera. If you have a 3.3 V USB serial adapter and are happy to open the phone, even better: see "Serial" below, but it is not needed.

**Steps**

1. Photograph the label under the phone, and in Yealink's menu the Status page (firmware and hardware version).
2. Unplug the power (or the network cable, if it powers the phone).
3. Hold **Speakerphone**, plug the power in, and keep holding until the screen shows a menu with "1. TFTP" (about 5 seconds). Let go. Do not press OK at this menu. Photograph it.
4. Press **1**. Enter the phone's address, the netmask, the gateway and the PC's address, as it asks. Photograph what it shows next.
5. The PC's TFTP server shows the phone fetching a `.rom`, a `.bin` and a `.rfs`. Keep that log.
6. Within a minute the screen should show text scrolling, then a clock. Photograph the screen at each stage, whatever it shows, especially if it is upside down, has wrong colours, is shifted, or stays as it was.

**From a USB stick instead.** The zip's `README.txt` has these steps. Copy the three files to the top folder of a FAT32 stick (one partition, MBR), put it in the phone, do steps 1 to 3, press **2** instead of 1, and carry on at step 6. Use a 5 V supply of 1.5 A or more ([live-image.md](live-image.md#the-stick)). There is no TFTP log this way, but Yealink's settings survive if the stick is out of the phone before its next start: with the stick left in, Yealink's firmware tries to update from it and resets them. All of that is known from the T48S; a T46S's U-Boot has not been seen reading a stick.

**Checks, in this order; stop at the first that fails and tell us what you saw**

| # | Do | Expect |
|---|---|---|
| 1 | look | the clock screen, the right way up, five buttons along the bottom. Top left: a green square and the phone's address. Top right: a red square and "No service" (if that square is blue, the colours are swapped: tell us). A note says it was started in recovery mode and that nothing is installed |
| 2 | press each key once: digits (a digit opens the dialler; X rubs out), arrows, OK, X | Up: Calls. Left: Contacts. Down: Settings. OK: Apps. X: back |
| 3 | lift the handset, put it back | the call screen appears and goes |
| 4 | Left (Contacts), then the four soft keys under the screen, one at a time | soft key 2 opens "Add": type a name on the dial keys, OK, a number, OK |
| 5 | Down (Settings), Screen, change the brightness with Left and Right | the screen gets dimmer and brighter, no flicker |
| 6 | X back to the clock | the address at the top left is one from your DHCP server. Ping it from the PC |
| 7 | Volume up and down | a level shows |
| 8 | if you have a SIP account: Settings, Accounts, enter it; call a number with the handset, then with Speakerphone | sound both ways. Start quiet |
| 9 | Settings, Access: turn SSH on with your public key if you want to send us logs (`dmesg`, `/var/log/t48/shell/current`, `cat /sys/module/t48s_holtek/parameters/hwversion`) | |

The line keys do nothing, and no key lamps light: that is expected in this set.

**Going back.** From a stick: unplug the power, take the stick out, plug the power in. Over the network: stop the TFTP server first (Yealink's firmware asks it for the `.rom` on its next start; with `--rom-once` ours already refuses). Unplug the power, plug it in, hold nothing. Yealink starts, with its settings reset as said above. If it sits at the logo for more than two minutes, unplug and plug in once more.

**If it goes wrong**

- No menu: Speakerphone was let go too early. Unplug and try again.
- "Update failed" or it stops before fetching the `.bin`: nothing has changed in the phone. Photograph the message, unplug, start normally. The TFTP log shows what it asked for.
- The screen goes blank or freezes after the files are fetched: our kernel did not come up. Unplug, start normally. A photo of the last thing on the screen is the most useful thing you can send.

**Serial, for those who can.** On the T48S the console is a 3-pin header marked CON4 on the main board: ground, the board's transmit, the board's receive, 3.3 V levels, 115200 8N1. We do not know where it is on a T46S. If you open the phone, photograph both sides of the board first; look for an unpopulated 3 or 4-pin header near the SoC, find ground with a meter, and the pin that idles at 3.3 V and flickers at power-on is the board's transmit. Never connect an adapter's 5 V or 3.3 V supply pin. A capture from power-on to the clock (U-Boot's banner, then our kernel's log) would answer most of the open questions at once.

**What to send back**, on issue #10: the photos, the TFTP log, which checks passed, and anything odd.

## Open questions a first start answers

1. Does a T46S's U-Boot accept our label and ask for `T46S.*`? (The TFTP log.)
2. Is RAM 64 MB, and does U-Boot leave the screen running at 0x03f80000 in 16 bits? (A picture at all.)
3. Upside down or not, and red and blue the right way round? (The photos.)
4. Is the key table right? (Check 2 and 4.)
5. Backlight pin and curve. (Check 5.)
6. Do Ethernet and sound start as on the T48S? (Checks 6 and 8.)
7. U-Boot's version and the hardware version string. (Serial, or SSH.)
