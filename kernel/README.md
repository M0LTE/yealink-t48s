# Our kernel for the T48S

**Status (2026-11-22): boots to a busybox shell on the serial console.** U-Boot's recovery mode (option 1, TFTP) loads it with our ramdisk; it prints the full boot log on ttyAMA0, mounts the ext2 ramdisk, runs our init, draws a banner on the screen through `/dev/mem`, and gives a working shell (`t48s#`). Timekeeping is right (`sleep 10` takes 10.007 s); BogoMIPS reads 498.89, matching Yealink's 498.07 (see "CPU speed" below). Ethernet works (our driver, below); the kexec build gets an address by DHCP and runs dropbear for key-only SSH. The screen, keys, hook switch and touchscreen have drivers too (below); and the microphones (ALSA capture, below); none yet for NAND or audio playback.

Linux 3.6.11 (the last mainline release with Broadcom's `mach-bcmring` platform, the BCM11107 family the phone's BCM11188 belongs to), patched and configured for the phone. Built with gcc 4.7 in an Ubuntu 14.04 container.

- `build.sh`: applies the board patches and builds `T48S.bin` (U-Boot legacy image, load and entry 0x0E000000 like Yealink's kernel). Run inside the container from a directory holding `linux-3.6.11/` and this `kernel/` directory:
  `docker run --rm -u $(id -u):$(id -g) -v $PWD:/w -w /w t48s-kbuild:trusty sh kernel/build.sh`
- `t48s.config`: config added on top of `bcmring_defconfig`.

Container (on hyperv-gha, `~/t48s-kbuild`):

```
FROM ubuntu:14.04
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq gcc gcc-arm-linux-gnueabi make bc u-boot-tools xz-utils bzip2 patch
```

Board changes:

| What | Mainline | T48S |
|---|---|---|
| Machine number | 2289 | 2342 (what U-Boot passes) |
| RAM | 256 MB from 0 (forced by the platform fixup, which also drops U-Boot's tags and command line) | 0x00400000 to 0x07A20000, stopping below U-Boot's framebuffer at 0x07A24000 |
| UART clock | 142.857 MHz | 100 MHz, measured from the divider U-Boot programmed |
| PLL spread spectrum | enabled at boot | left as U-Boot set it |
| Flash (MTD/NAND) | built in | not built: this kernel cannot write the NAND |
| Command line | from U-Boot | built in: serial console on ttyAMA0 at 115200, ramdisk at 0x02a80000 |
| Delay loop | wherever the link puts it | on an 8-byte boundary (see "CPU speed" below) |
| Early debug output | none for this platform | `mach/debug-macro.S` for UARTA (phys 0x80012000, virt 0xF8012000) |
| Static I/O map | UMI and NAND entries overlapping CHIPC | dropped (3.6 BUG()s on overlaps) |
| UART clock lookup | registered for `uarta`/`uartb`, devices named `uartA`/`uartB` | names fixed; use count starts at 1 so the AMBA ID read does not gate the clock off under the console |

Pitfalls found on the way, in boot order: RAM must end on a 1 MB boundary or 3.6 hangs in `paging_init`; overlapping static mappings hit `BUG()` in `vm_area_add_early`; a UART clock gated off during `amba_device_add` leaves the early console spinning on a full FIFO (a silent hang).

Root filesystem from Buildroot (issue #16): `buildroot/` is a Buildroot external tree (pinned release, musl, ARM1176 soft-float, 3.6 headers, everything static) that builds the same userspace as the hand list: busybox with DHCP and NTP, dropbear (key login only), getty on the screen, our programs from `native/`, plus curl with mbedTLS, CA certificates and mtd-utils. `buildroot/build.sh WORKDIR` leaves `WORKDIR/output/images/rootfs.cpio`; add `sdk` for the SDK tarball. To build it into the kexec kernel, put `rootfs.cpio` beside `linux-3.6.11/` and run

```
docker run --rm -u $(id -u):$(id -g) -v $PWD:/w -w /w -e ROOTFS=rootfs.cpio t48s-kbuild:trusty sh kernel/build.sh kexec
```

`kexec-rootfs/root.authorized_keys` is added as root's SSH keys if it exists (it is gitignored, never published); without it there is no SSH, only the serial console. `build.sh` patches a fresh `linux-3.6.11/` each time. CI (`.github/workflows/build.yml`) builds the root filesystem, SDK and kernel image on pushes to main and publishes them on a `v*` tag. The container's recipe is `kernel/Dockerfile`.

Userspace for testing: `tools/mkrfs-bb.sh` builds `T48S.rfs` with a static busybox (musl), a shell on the serial console and `native/membanner` drawing on the screen. Boot it with U-Boot's recovery mode, option 1 (TFTP), served by `tools/tftpd.py DIR 69 --rom-once`.

## Memory map and DMA on the BCM11188 (2026-11-22)

Yealink's page tables (`kmod/pgdprobe.c`, output in `dumps/yealink-iomap.txt`) confirm mainline's `mm_addr.h`: AXI blocks at 0x30100000 (ARAM), 0x30200000 (DMA0), 0x30300000 (DMA1), 0x30400000 (Ethernet switch), 0x30500000 (LCD), 0x30600000 (APM), 0x30700000 (SPUM), 0x30800000/0x30A00000 (VPM); chip control at 0x80000000 (ChipId reads 0x111880B0, BCM11188 rev B0). Ignore the addresses in the comments in mainline's `mm.c` (0x32000000 etc.), they are stale.

DMA0 (Synopsys DW AHB DMAC, ID 0x44571110) carries audio: Yealink's firmware runs four of its channels between RAM and the APM codecs (the kexec loader stops them before the jump). Our kernel's DMA framework neither initialises its channels nor takes their interrupts; `drivers/t48s_audio.c` programs the two it uses (3 and 5) directly and touches no other. DMA1 carries the Ethernet switch (channel 2 RX, channel 3 TX); its channels are stopped before being reset.

## Ethernet (2026-11-22)

`drivers/t48s_eth.c` (copied into `drivers/net/ethernet/` by `build.sh`) drives the CPU port of the on-chip switch. Frames go through the switch's PTM (receive) and MTP (transmit) FIFOs, fed by DMA1 channel 2 (receive, a ring of 32 linked-list items) and channel 3 (transmit, two items per frame: a config word, then the frame). Every frame on that port carries a 4-byte Broadcom tag after the source address and a 4-byte FCS slot at the end; the driver strips and adds them.

The DMA control and config values are the ones Yealink's kernel was seen using (read live with `kmod/regprobe.c`): RX CTL_LO 0x1a408037, CFG_HI 0x46, status from 0x30480010; TX CTL_LO 0x18901036 (0x...37 on the last item), CFG_HI 0x826, status to 0x30490010. Switch CPU interface: PTM config 0x90040010, MTP config 0x90000013. These match Broadcom's GPL "Island" driver for the same switch IP. Switch page registers are 64 bits wide (accessed with ldrd/strd) at base + ((page << 8) + offset) * 8.

The driver assumes the switch is already set up (it is, after a kexec from Yealink): ports forwarding, a static ARL entry for the phone's MAC on the CPU port. It only re-enables the CPU port, which Yealink's `ifconfig eth0 down` turns off before the jump. It uses the phone's own MAC, so the DHCP reservation gives the usual address. Buffers are DMA-coherent and every frame is copied; one transmit is in flight at a time. Simple, and fast enough for SSH.

`dma_device.c` marks the two ESW channels `NO_ISR` so the DMA framework leaves their interrupts to the driver.

## Keys, hook switch and the Holtek chip (2026-11-22, issue #3)

`drivers/t48s_keypad.c` (copied into `drivers/input/`) gives two input devices. "T48S keypad" (`/dev/input/event0`) reads the SoC keypad controller, which U-Boot and Yealink leave running; "T48S hook switch" (`/dev/input/event1`) reads GPIO29 and sends `KEY_PHONE`, held while the handset is lifted. Both are polled every 20 ms: the keypad interrupt's acknowledge is still a guess and this kernel has no GPIO interrupts. The keys also type on the screen console (digits, `*`, arrows, OK = Enter, X = Backspace).

The key table is in the driver, all confirmed by pressing every key (2026-11-22): digits, `*` (`KEY_KPASTERISK`), `#` (`KEY_NUMERIC_POUND`, which the console does not type), arrows, OK (`KEY_ENTER`), X (`KEY_BACKSPACE`), Volume -/+ (`KEY_VOLUMEDOWN`/`KEY_VOLUMEUP`), Headset (`KEY_PROG1`), Mute (`KEY_MUTE`), Message (`KEY_MESSENGER`), Hold (`KEY_PAUSE`), Redial (`KEY_AGAIN`), Transfer (`KEY_FORWARD`), Speakerphone (`KEY_PROG2`). `native/keytest` names each key on the screen. The table can be changed at run time with the usual `EVIOCSKEYCODE`.

`drivers/t48s_holtek.c` (copied into `drivers/misc/`) bit-bangs the Holtek chip on GPIO36/11/10. At boot it reads the version block and, if the chip answers as a protocol 2.0 one, turns its watchdog off; the kexec loader has already done that, so this is for boot paths that have not. `echo N > /sys/module/t48s_holtek/parameters/watchdog` arms it for N seconds (nothing feeds it: the phone resets, which is a handy way to prove it works), 0 turns it off. The chip's LED and the other LEDs are not driven yet.

Not tested yet: an actual key press. To test:

```
tools/ourkernel.sh ssh 'echo 1 > /sys/module/t48s_keypad/parameters/debug'
```

then press each key once. Every press and release prints a line on the screen and in `dmesg`, such as `t48s_keypad: row 8 col 0 code 65 down -> key 2` (key 2 is `KEY_1`; `key 0` means the position is not in the table). Lift and replace the handset: expect `hook GPIO29 = 0 (off hook)` then `= 1 (on hook)`; if it is the other way round, flip `HOOK_OFF_LEVEL` in the driver. Note the code printed for Headset, Mute, Message, Hold, Redial, Transfer and each volume key so the table can be finished. With debug off (`echo 0`), digits typed at the `login:` prompt on the screen should appear there.

## Touchscreen (2026-11-22, issue #4)

`drivers/t48s_touch.c` (copied into `drivers/input/`) gives the input device "T48S touchscreen" (`/dev/input/event2`): `BTN_TOUCH`, `ABS_X`, `ABS_Y`, `ABS_PRESSURE`, all raw 12-bit values (0 to 4095), the same as Yealink's own driver reports. The chip is a TI TSC2046 on the SoC's SPI host (an ARM PL022). The driver talks to the PL022 directly and polls: about 50 times a second (it asks for 10 ms, the 100 Hz tick makes that 20) it reads the pen-down pin (GPIO26), and only while that is low does it clock the chip (X ten times, Y ten times, pressure, in one quarter-millisecond transfer). No SPI core, no GPIO interrupts, nothing to configure.

Raw values become pixels in userspace, with a tslib-style `/etc/pointercal`. The kexec root filesystem carries the phone's factory calibration (from U-Boot's `calparam`): `14798 13 -4134128 53 10207 -5441552 65536`, meaning x = (14798 * rawx + 13 * rawy - 4134128) / 65536 and y = (53 * rawx + 10207 * rawy - 5441552) / 65536. To recalibrate later, replace those seven numbers.

Checks without touching anything:

```
tools/ourkernel.sh ssh 'cat /sys/module/t48s_touch/parameters/diag'
```

prints the pen pin, the chip's temperature readings (about 37 C on the bench), and counters. To test with a finger:

```
tools/ourkernel.sh ssh touchtest
```

(`native/touchtest.c`, built into the root filesystem). Tom ran this on 2026-11-22: taps in the four corners and a drag all landed under the finger with the factory calibration. Each touch prints the raw values and the pixel position, and a white dot is drawn on the screen there; it should land under the finger. `echo 1 > /sys/module/t48s_touch/parameters/debug` logs every reading, including rejected ones, to `dmesg`.

## Microphones (2026-11-23, issues #6 and #24)

`drivers/t48s_audio.c` (copied into `sound/arm/`, `CONFIG_SND_T48S`) is an ALSA capture driver for the two codecs in the SoC's APM block: card "T48S", `hw:0,0` codec A and `hw:0,1` codec B, S16_LE mono 16 kHz, with mixer controls for the capture side of each. No DSP and no vendor module. It does not play anything and does not initialise the APM block from cold yet (it relies on Yealink having done that before the kexec jump). `docs/audio.md` has the details, the measurements, and a warning about codec B's controls and sidetone.

```
tools/ourkernel.sh ssh 't48rec -d 1 -s 5 -o /tmp/b.wav'    # record 5 s from codec B, report rate and level
tools/ourkernel.sh ssh t48mix                              # list the controls
tools/ourkernel.sh ssh 'cat /proc/asound/card0/t48s'       # counters and registers
```

## CPU speed (2026-11-22, issue #9)

Our kernel used to report 331.77 BogoMIPS against Yealink's 498.07. The CPU was never slower: the core runs at about 500 MHz under both. On this ARM1176 the two-instruction delay loop takes 2 cycles per pass when it starts on an 8-byte boundary and 3 when it starts 4 bytes off one, and our build happened to link it at 0xc01034cc where Yealink's sits at 0xc014bab0. `build.sh` now aligns it; BogoMIPS reads 498.89.

`native/cpubench.c` runs the same work under either kernel (CPU time, best of three passes; Yealink's figures are a little lower because its UI and DSP keep disturbing the caches):

| Test | Yealink 2.6.27 | Ours 3.6.11 |
|---|---|---|
| delay loop, 8-byte aligned (million passes/s) | 235 | 250 |
| delay loop, 4 bytes off | 156 | 167 |
| integer mix in L1 cache (million passes/s) | 27.0 | 31.2 |
| memcpy through 4 MB buffers (MB/s) | 160 | 200 |

The CP15 control register is 0x00c5387f under Yealink and 0x00c5387d under ours: caches, branch prediction and MMU are on in both; the one differing bit is alignment-fault checking, which does not affect speed.
