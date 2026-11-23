# Live image: try our firmware without flashing

Issue [#7](https://github.com/M0LTE/yealink-t48s/issues/7). This is the "try it" path from [architecture.md](architecture.md): three files on a TFTP server, U-Boot's recovery mode, nothing written to the phone's flash. Another T48S owner can boot our firmware once, from RAM, and a normal power cycle brings their phone back.

The route itself is proven (own-kernel.md, 2026-11-22): our kernel and root filesystem boot through recovery option 1. What issue #7 adds is a ROM we are allowed to share (not Yealink's firmware), a clean story for the factory-reset trap, and the driver init needed when Yealink never ran.

## The three files

Recovery mode (hold Speakerphone at power-on, choose 1 for TFTP) fetches files named after the board:

| File | What we ship |
|---|---|
| `T48S.rom` | a small crafted header, built by `tools/mkliveimage.py`. Not Yealink firmware |
| `T48S.bin` | our kernel (U-Boot legacy image), `kernel/build.sh` |
| `T48S.rfs` | our root filesystem as an ext2 ramdisk (max 8 MB; ours is about 3.4 MB) |

Build the set and serve it:

```
tools/mkliveimage.py build OUT bin=T48S.bin rfs=T48S.rfs
sudo tools/tftpd.py OUT 69 --rom-once
```

`--rom-once` stops serving the ROM after the ramdisk has gone, so Yealink's resume script (below) cannot use it to reflash.

## What U-Boot checks in the ROM

U-Boot fetches only the head of the ROM (about 2936 bytes, then aborts the transfer with "File too large" on purpose), checks it, prints `Rom check ok`, and boots the `.bin` and `.rfs`. The check is in U-Boot at 0x03B15274; it was read out of the disassembly and confirmed byte for byte against a genuine 66.82.0.31 ROM (the crafted-ROM generator reproduces that ROM's stored CRC exactly). There is **no cryptographic signature** anywhere; the whole gate is a CRC plus model and version fields.

The header is 128 bytes of big-endian 32-bit words:

| Offset | Field | How U-Boot uses it |
|---|---|---|
| 0x00 | magic | must equal `0xAD24EC0B`, else `Rom magic Err` |
| 0x04 | header length | how many bytes the CRC covers (0x80) |
| 0x08 | header CRC32 | `crc32` over the header with this field zeroed, seed 0, ordinary IEEE; else `Rom crc Err` |
| 0x0c | tool/format | not checked against the board |
| 0x10 | flags | bit 0 "force" skips the hwid and swid checks; bit 16 "skipoem" skips the OEM check |
| 0x14 | body length | informational; U-Boot never reads the body |
| 0x1c | module count | not checked (U-Boot runs no module loop; that is the userspace updater, which we never reach) |
| 0x20 | primary hwid | compared to the phone's hwid; if different, U-Boot scans the list at 0x64 |
| 0x34 | OEM name | `strcmp` against the phone's `phoneoem` env, unless skipoem; else `Rom OEM Err` |
| 0x44 | version string | not checked |
| 0x5c | hw revision | must be >= the phone's, unless force |
| 0x60 | swid | must be >= the phone's `phonesid` env, unless force |
| 0x64 | supported-hwid list | 16 bytes; the phone's hwid must appear here (or at 0x20) |

Hardware ids: 65 = T48S, 66 = T46S, 67 = T42S, 68 = T41S, 69 = T27G, 70 = T54S, 74 = T52S. Yealink's own T46S ROM lists 65, 67, 68 at 0x64 ("ACD" in ASCII), which is exactly why that one ROM already boots on a T48S.

### The crafted ROM

`tools/mkliveimage.py` writes a header with the right magic and CRC, sets **force** and **skipoem** (so hwid, swid and OEM are never compared; those differ between firmware versions and resellers), and lists every T4xS hwid at 0x20 and 0x64 so the type check passes on any of them. The body is zero padding. It carries none of Yealink's code.

Validate any ROM offline, emulating U-Boot's exact logic:

```
tools/mkliveimage.py check OUT/T48S.rom 65     # 65 = the phone's hwid
```

This prints each check and `PASS`/`REJECTED`. It accepts the genuine ROM and the crafted one on hwids 65-74, and rejects an unrelated model.

## The factory-reset trap, exactly

The danger is not U-Boot and not our image; it is Yealink's own firmware on the **next** boot. Here is precisely what happens and why.

**What U-Boot writes during a TFTP recovery boot.** In the recovery updater (0x03B15A18), after you enter the network details and the ROM check passes, U-Boot does, to its environment partition (`userenv`, mtd3):

- `setenv ipaddr / netmask / gatewayip / serverip` to the values you typed,
- `setenv BOOTMODE resumemode`,
- `saveenv` (it erases and rewrites `userenv` a few times along the way),
- then `run boottftp`, which loads our `.bin` and `.rfs` and boots them.

So after the live boot, `BOOTMODE=resumemode` is sitting in the phone's `userenv`. Nothing else on the flash is touched. (U-Boot sets `BOOTMODE=usbmode` instead on the USB path, and `safemode` only on its own error paths.)

**What Yealink does with it on the next normal boot.** Yealink's `rcS` reads `BOOTMODE`, clears it, and runs the matching script. `BOOTMODE=resumemode` runs `resumemode.sh`, which:

1. runs `reserve.sh`, and
2. `upgrade -u tftp://$serverip/T48S.rom` to reflash over TFTP.

`reserve.sh` is the reset: it copies four whitelisted paths out of `/config` (`certs/phonedev`, `Setting`, `certs/uc`, `vpPhone`), then **`flash_eraseall /dev/mtd11`** (the whole `cfg`/`/config` partition), remounts it and copies only those four back. Everything else in `/config` is gone: the account, timezone, language, web password, and any boot hook of ours. That is the "factory reset" we saw on 2026-11-22. The `upgrade` step then tries to pull `T48S.rom` from the server IP U-Boot stored; with `--rom-once` the server refuses it, so the reflash fails harmlessly, but the `reserve.sh` wipe has already happened.

So any boot that enters `resumemode` resets the phone's settings, whether or not the reflash succeeds. The only safe outcome is to never let `resumemode` run.

### How to leave the phone exactly as it was

`BOOTMODE` lives in NAND (`userenv`). Our own kernel's flash driver is **read-only by design** (issue #5), so our live image cannot clear it without a flash write, and flash writes are gated on Tom's go-ahead. Three ways to handle this, in order of preference:

1. **Clear `BOOTMODE` from the live image (one surgical userenv write).** This is what `tools/rescue-init.sh` already does, using Yealink's own `userenv` tool, when the live `.bin` is Yealink's kernel. For our own kernel it needs a single, address-restricted NAND write to the `userenv` partition only (clear `BOOTMODE`, and `fwup_mode`/`fwup_install` if set). This is **a decision for Tom**: it is the one deliberate flash write on the "try it" path, it is small and well understood, and it is the difference between a clean try and a settings reset for every other owner. Listed below.
2. **Boot the rescue `.bin` first.** Ship Yealink's kernel as the `.bin` with `tools/rescue-init.sh` as the ramdisk (the existing `tools/mkrescue.sh` set), which clears `BOOTMODE` and resets through the watchdog back into a normal boot. That is the clean "undo", but it is a separate boot, not our firmware.
3. **Accept the reset and document it.** If we write nothing, tell the owner plainly: trying the live image once will reset their phone's Yealink settings on the next power cycle. For a borrowed test phone that is fine; for someone's working desk phone it is not, which is why option 1 is the real answer before this goes public.

Until Tom decides, the live image is honest about it: option 3's wording goes in the user guide, and `tftpd.py --rom-once` at least stops an unwanted reflash.

## Cold start: drivers when Yealink never ran

On a recovery (TFTP) boot the chain is **boot ROM -> U-Boot -> our kernel**; Yealink's kernel never runs, so anything Yealink's drivers used to set up is either left by U-Boot or not set at all. Status per driver (the kexec-from-Yealink path is unchanged in every case; each driver detects "already running" and only initialises what it finds cold):

| Driver | On the live (U-Boot only) path | Cold-start code |
|---|---|---|
| LCD (`t48s_fb`) | U-Boot sets the PL111 up and keeps scanning out the framebuffer | none needed; works cold |
| Keypad (`t48s_keypad`) | U-Boot runs the keypad controller (it scans keys for the recovery menu), so it is found enabled | `t48s_keypad_start()` sets pin mux, bus clock, prescaler and control if `KEYC_CTRL_EN` is clear. Present, guarded, **untested from true cold** |
| Hook switch | GPIO29 read only | works cold |
| Backlight (`t48s_backlight`) | U-Boot leaves the lamp on at level 8; the driver starts from what it finds | works cold |
| LEDs (`t48s_leds`) | U-Boot state unknown; the driver sets the mux to plain GPIO, directions to output and the pins high unconditionally | cold-safe already |
| Touch (`t48s_touch`) | U-Boot does not use SPI, so this is the real cold case; the driver sets the pin mux, SPI-host bus clock, chip-select and pen-down direction unconditionally, then probes the PL022 | present; **untested from true cold** (U-Boot may leave the SPI clock gated) |
| Ethernet (`t48s_eth`) | U-Boot's own TFTP ran, so the switch and DMA1 are already up on this exact path; the driver re-asserts IMP control (issue #34) and checks it every 2 s | works on the live path; a full switch bring-up with no U-Boot net is not written and not needed here |
| Audio APM (`t48s_audio`) | **truly cold.** U-Boot does no audio, so the APM codecs are not enabled. The driver detects this (`APM control >> 28 != 0xf`) and warns "cold start is not supported yet" | **not implemented.** Needs the APM reset, clock, sample-rate, FIFO and codec-enable sequence, and the voice-DSP firmware for echo cancelling. The idle register snapshot is in `dumps/audio/apmregs-yealink-idle.txt`; the sequence is in Yealink's `halaudio_apm.ko`. Blind replay is unsafe without the phone |

So the live image **boots and shows the UI, keypad, touch, screen, LEDs and network** with no Yealink. **Audio (calls) needs the APM cold-start**, which is a hardware-test job; it does not block "try the phone's interface".

### What must be tested on the phone

1. Touch from cold: does the PL022 probe and do taps land, with U-Boot never having touched the SPI clock? (The `/sys/module/t48s_touch/parameters/diag` temperatures tell you the SPI link is alive.)
2. Keypad from cold: only if U-Boot is ever found with the controller off; normally it is on.
3. Ethernet: does U-Boot's switch/ARL/forwarding state carry into Linux so unicast replies arrive, or is a learn needed? `tools/arp-test.sh` and the eth `diag` parameter show it.
4. Audio APM cold-start: the whole sequence, once written, on the phone. Nothing else needs it.

## Test plan for a daytime session

Goal: boot the live image on the test phone over TFTP, see the UI come up with no Yealink, confirm the recovery steps, and (if Tom approves option 1) confirm the phone comes back clean.

Minimum key presses:

1. On the server: `sudo tools/tftpd.py OUT 69 --rom-once` (OUT holds the three files; watch its log).
2. Start a serial capture on CON4 (ttyAMA0, 115200) **before** powering the phone, per the RX-first rule.
3. On the phone: hold **Speakerphone**, reconnect power, keep holding until the screen shows "1. TFTP / 2. USB". Do **not** hold OK.
4. Press **1** (TFTP). Enter IP, netmask, gateway, and the TFTP server IP with the keypad; OK to confirm.

Watch for, in order: `scan key=40` then `Start Updating...` on screen; in the TFTP log, a short `T48S.rom` fetch, then `T48S.bin` and `T48S.rfs`; on serial, our kernel's boot log and a shell banner; on screen, our UI. `Rom check ok` on serial confirms the crafted ROM passed.

Recovery (always available, because we never touch U-Boot or its recovery): just **power-cycle without holding any key**. What that does depends on the `BOOTMODE` decision:

- If option 1 is approved and the live image cleared `BOOTMODE`: a normal Yealink boot, settings intact.
- If not: the first power cycle runs `resumemode.sh` (screen shows "Firmware updating..."), the ROM download is refused by `--rom-once` so it fails, and the phone comes up with its Yealink settings reset. A second power cycle is a normal boot. This is the behaviour to confirm and document, not a fault.

If the screen freezes or the phone reboots mid-boot, that is a watchdog; note the serial log's last line. Nothing here can brick the phone: recovery mode lives in U-Boot, which we never write.

## Decisions for Tom

1. **The one userenv write (option 1 above).** Allow the live image to clear `BOOTMODE` (and `fwup_mode`/`fwup_install`) with a single address-restricted write to the `userenv` partition, so trying our firmware does not reset another owner's Yealink settings. This is the only flash write on the "try it" path and the thing that makes it safe to share. Without it, the user guide must warn that a try resets Yealink settings.
2. **How the live image finds `T48S.conf`.** The plan is TFTP from the DHCP boot server (option 66) after the phone has an address, same name as the other files. Confirm that is acceptable, or prefer embedding settings in the `.rfs` for the first release.
3. **What to ship as the public `.rfs`.** Our Buildroot root filesystem with SSH off until turned on at the screen (keys may come from `T48S.conf`), per docs/access.md.
