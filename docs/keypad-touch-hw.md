# Keypad, hook, LEDs and touch: how Yealink's modules drive the hardware

Offline reverse engineering of `dumps/keypad.ko` (v1.11.5.17, 2017) and `dumps/tsc.ko` (v1.3.0.3, 2016), with cross-checks against Yealink's kernel image (`dumps/vmlinux-image.bin`) and U-Boot (`dumps/uboot.dis`). For issues #3 (keypad/hook/LEDs) and #4 (touch). Nothing here was tested on the phone.

References are `function+offset` in the module (`arm-linux-gnueabihf-objdump -drz`), or a virtual address in Yealink's kernel (loaded at 0xC0008000; exported symbol names recovered from its `__ksymtab`). Guesses are marked **guess**.

## Summary

| Function | Hardware | Bus |
|---|---|---|
| Keypad matrix | SoC keypad controller (KEYC) at 0x8000D000, IRQ 48 | on-chip, pins GPIO43-58 |
| Hook switch | GPIO29, both edges, IRQ 129 (GPIO IRQ) | plain GPIO |
| Touch | TI TSC2046 (ADS7846 type) resistive controller | SoC SPI host (PL022 type) at 0x80015000, CS on GPIO25, pen IRQ on GPIO26 |
| Watchdog, one LED, hw version, anti-clone check | Holtek MCU | bit-banged 3-wire serial on GPIO36 (CS), GPIO11 (CLK), GPIO10 (data) |
| Other LEDs | GPIO3, GPIO4, GPIO5, active low (if hw id is 35, see below) | plain GPIO |
| LCD backlight | SoC PWM at 0x80002040 on GPIO35, enable on GPIO18 | on-chip |

The keypad is **not** read through the Holtek chip. The Holtek event interrupt is configured as "none" in this build, so the MCU never reports keys; U-Boot's "scan key" also reads KEYC directly.

## Address and IRQ conventions (Yealink's 2.6.27 kernel)

- Virtual = phys with the top nibble moved: 0x8000xxxx is used as 0xF800xxxx (`dumps/yealink-iomap.txt`). All addresses below are physical.
- GPIO block: bank 0 (GPIO0-31) at 0x80004000, bank 1 (GPIO32-60) at 0x80004800. One bit per pin. Registers (from the gpiochip ops at 0xC0047C7C..0xC0047DAC and the irq code at 0xC00312E4..0xC0031704, 0xC0044578):
  - +0x000 data (read input, write output)
  - +0x400 direction, 1 = output
  - +0x404 irq level (1 = level, 0 = edge)
  - +0x408 irq both-edges
  - +0x40C irq polarity (1 = high / rising)
  - +0x410 irq enable
  - +0x418 irq status
  - +0x41C irq clear (write 1)
- GPIO IRQ numbers are 100 + gpio (mainline `irqs.h` agrees: `IRQ_GPIO_0 = 100`). They are demultiplexed from INTC1 lines 32 (bank 0) and 33 (bank 1). **Mainline 3.6 has no GPIO irq chip**, so a 3.6 driver needs one (chained on IRQ 32) or must poll.
- Pin mux: chipc `GpioMux` registers at 0x80000118 + 4*(pin/8), 4 bits per pin at bit (pin%8)*4 (Yealink `gpiomux_request` at 0xC0041EC8; matches mainline `chipcHw_REG_GPIO_MUX`). Functions: 0 GPIO, 1 KEYPAD, 3 SPI, 5 LEDMTXP, 13 MISC.
- Bus clock gates: chipc `BusIntfClock` at 0x8000005C. Bit 15 = keypad (LDK), bit 16 = LED/PWM, bit 22 = SPIH.

## Hardware id (phone_hwversion) decides most of it

`keypad.ko` reads 7 bytes of hardware version from the Holtek MCU at load and copies them into the kernel symbol `phone_hwversion` (`init_module+0xb4..0xcc`). Byte 6 is the product id; `keypad_dev_init`, `led_init`, `bklight_init`, `misc_init` and `tsc.ko` all switch on it. U-Boot also passes it on the command line as `hwversion=`.

`tsc_dev_identfy+0x2c..0x84` accepts only ids 0x23 (35), 0x33 (51), 0x37 (55) and 0x41 (65), so the T48S is one of those. **Guess: 35.** Reason: for id 65, `led_init` puts an LED matrix on GPIO16-21, which collides with the touch controller's GPIO21 and 16; for id 35 everything is consistent. Keypad, hook, backlight and touch are the same for 35 and 65; only the LEDs differ. First live check: `cat /proc/keypad/holtek_hw` (or the boot line `Hardware version:`, printed byte 6 first).

## Keypad: SoC keypad controller (KEYC)

Source: `keypad_dev_init` (0x6f38), `keypad_hw_isr` (0x73d0), `keypad_dev_exit` (0x6e6c); U-Boot does the same at 0x3B209F4.

- Base 0x8000D000, IRQ 48 (INTC1 bit 16, mainline `IRQ_KEYC`), `request_irq(48, keypad_hw_isr, 0, "T4x-keyboard")` at +0x13c.
- Matrix size for ids 35 and 65 (`keypad_dev_init+0x200`): 9 lines starting at GPIO43 (GPIO43-51, the "row" bit index) and 7 lines starting at GPIO52 (GPIO52-58, the "column" index). Id 37 uses 5x5 from 47/52, id 41 uses 9x8 from 43/52. Which set drives and which senses is not visible in software (**unknown**, the block does it).
- Registers:
  - +0x00..+0x10: five status words. Each word holds two columns of 9 bits: column 2i is bits 16-24 of word i, column 2i+1 is bits 0-8. A **0 bit means pressed**. (`keypad_hw_isr+0x30..0x64`: masks 0xFE00FE00 off, stores word>>16 then word&0x1FF.)
  - +0x14: scan prescaler = bus_clock / 81000 - 1, 16 bits (`+0x350..0x388`, `chipcHw_getClockFrequency(10)` = BUS clock; U-Boot computes bus/9000/9 - 1).
  - +0x18: written 200 (**guess**: debounce or scan period).
  - +0x1C: control. Bit 0 = enable, bit 1 = interrupt enable, bit 3 cleared and bit 4 set at init (**meaning unknown**).
- Init sequence (`keypad_dev_init+0x16c..0x3e4`, same order in U-Boot):
  1. mux GPIO43-58 to function 1 (KEYPAD)
  2. BusIntfClock |= 0x8000
  3. CTRL &= ~1; CTRL &= ~8; CTRL |= 0x10
  4. REG18 = 200; REG14 = bus/81000 - 1
  5. CTRL |= 1; CTRL |= 2; enable_irq(48)
- Interrupt handler: CTRL &= ~2, read the five status words, compare with the previous bitmap, report changes, CTRL |= 2. There is no other acknowledge write (**guess**: toggling bit 1 is the ack).
- Linux keycode = row*8 + col + 1, row = bit 0-8, col = column 0-6 (`keypad_hw_isr+0xe4..0xf4`); value 1 = down, 0 = up.

Key positions from U-Boot's scan table (0x3B3A9CC; the id-65 table and the default table are identical). Scan code = what U-Boot prints at power-on.

| U-Boot scan | Key | row, col | keycode by the formula |
|---|---|---|---|
| 1 2 3 | 1 2 3 | 8; 0 1 2 | 65 66 67 |
| 4 5 6 | 4 5 6 | 7; 0 1 2 | 57 58 59 |
| 7 8 9 | 7 8 9 | 6; 0 1 2 | 49 50 51 |
| 10 0 11 | * 0 # | 5; 0 1 2 | 41 42 43 |
| 40 | Speakerphone | 4, 3 | 36 |
| 15 16 14 | ?, OK, ? | 6 7 8, 3 | 52 60 68 |
| 17 12 13 | X, ?, ? | 6 7 8, 4 | 53 61 69 |

**Conflict to resolve on the phone:** our earlier note (docs/status.md) says digits 1-9 give 57 58 59 / 49 50 51 / 41 42 43. By this table those are 4-9, *, 0, #, one row lower. Either that note is off by a row or my reading of the U-Boot table is. Pressing 1 and reading the code settles it.

## Hook switch: GPIO29

Source: `hook_init` (0x9de4), `gpio_hook_isr` (0x9f10), `hook_do_work` (0x9ea8).

- GPIO29 input, `request_irq(129, gpio_hook_isr, 3 = both edges, "on_hook")`.
- ISR masks GPIO29 in bank 0 +0x410, schedules work after 20 ms; the work reads GPIO29, reports keycode **89** with value = GPIO level (only if changed), and re-enables the bit in +0x410.
- At init the current level is reported once.
- Which level is off-hook: **unknown**, check live.

## LEDs

Source: `led_init` (0x8b90), `led_hw_set` (0x8474), `led_set` (0x8980).

The driver keeps a 64-bit LED word (`/proc/keypad/led`, ioctl). `led_hw_set`:

1. Bit 0 goes to the Holtek MCU (`holtek_set_led`, see protocol below: command 4, 32-bit value 0 or 1). **Guess**: power/message LED.
2. The word is shifted right by one. For ids 35, 41, 37 and 65 (`+0xbc..0xd4` -> `+0x464`): bit i goes to GPIO `gpio_led_map[i]` (.data 0x3c0 = GPIO3, 4, 5, 6), written as the inverse, so **active low**, for i < mcu_led_num.
3. For id 35, `led_init+0x1e0..0x2c0` requests GPIO3 (`led1_gpio`), GPIO4 (`led2_gpio`), GPIO5 (`led3_gpio`), sets them as outputs, unmuxes them to GPIO and drives them high (off); mcu_led_num = 3. So LED word bits 1, 2, 3 = GPIO3, 4, 5. Which physical key LED each one is: **unknown**.
4. For id 65 instead, `led_init+0x2c4` sets up the SoC LED matrix (`ledmHw_Init`, 0x80002000): rows GPIO16-21, columns GPIO0-9, mux function 5, LED clock enabled. Only relevant if the phone turns out to be id 65.

`led_flick` (blinking) is pure software timers on top of this.

## Holtek MCU: bit-banged serial

Source: `holtek_hw_gpio_init` (0x6788), `holtek_hw_gpio_send` (0x6bc8), `holtek_hw_gpio_recv` (0x68ac), `holtek_hw_gpio_set_freq` (0x6628); controller struct `HT_Controler` at .data 0x314.

Pins and timing (struct fields, offsets from 0x314):

| Field | Value | Meaning |
|---|---|---|
| +0x14 | 6000 | bit rate in Hz |
| +0x18, +0x1C | -1, -1 | interrupt GPIO and IRQ: none |
| +0x24 | 36 | CS (`ht_cs`), idle level 1 (+0x34), active low |
| +0x28 | 11 | CLK (`ht_clk`), idle level 1 (+0x40) |
| +0x2C, +0x30 | 10, 10 | data out (`ht_sdao`) and in (`ht_sdai`): one bidirectional pin |
| +0x38, +0x3C | 500, 500 | us after CS assert, us after CS release |
| +0x50 | 83 | half bit time in us, computed as (1000000/6000)/2 |

Frame, as one transaction (all bits MSB first):

1. SDA output, CS low, wait 500 us.
2. Command bits: for each bit, set SDA, wait 83 us, CLK low, wait 83 us, CLK high. (Data changes while CLK is high; the MCU presumably samples on the rising edge.)
3. Write: data bits the same way. Read: SDA to input, wait 83 us, then for each bit CLK low, wait 83 us, sample SDA (1 = high), CLK high, wait 83 us. (MCU shifts on the falling edge.)
4. Wait 83 us, SDA high, CS high, wait 500 us.

Init (`holtek_hw_gpio_init`): CS, CLK, SDA outputs at 1, then one dummy CS pulse (low 500 us, high 500 us). Multi-byte data is an n-bit integer sent MSB first; in the buffer it is stored little endian (the first bit on the wire is the top bit of the last byte).

Two protocol versions; `holtek_init` (0x3e98) tries 2.0 first, then 1.0. **Guess: the T48S speaks 2.0** (check `/proc/keypad/holtek_sw`, which prints 2.x for 2.0).

Protocol 2.0 (4-bit command), from the `proto_20_*` functions:

| Cmd | Dir | Data bits | Use |
|---|---|---|---|
| 0 | write | 16 | set encrypt challenge (then 2 ms) |
| 1 | read | 16 | get encrypt response |
| 2 | write | 16 | backlight (low byte max-v, high byte v; not used for the LCD on id 35) |
| 3 | write | 8 | **watchdog**, value in seconds, 0 = off (`proto_20_set_watchdog` 0x512c) |
| 4 | write | 32 | LED bits (only bit 0 is ever set, see LEDs) |
| 5 | write | 8 | "spanspeed" |
| 12 | write | 8 | PSTN control; init sends 0x10 |
| 14 | read | 64 | version block. Detect: byte 6 == 2. hw version: hw[0..3] = bytes 0..3, hw[4] = byte 7, hw[5] = byte 4 nibble-swapped, hw[6] = byte 5. sw version = bytes 6, 7. |
| 15 | read | 8 | event code (unused here: no interrupt) |

Protocol 1.0 (3-bit command): watchdog = cmd 1 + 8-bit value, valid 0-200; 201 sends 1 and spins forever (forced reset). LED = cmd 1 + 0xF6 (on) or 0xF7 (off). Version = cmd 3, read 32 bits.

Watchdog path: write to `/proc/keypad/watchdog` -> `keypad_proc_write_watchdog` (0x1274, `sscanf "%d"`, ignores negatives) -> `holtek_set_watchdog` (0x3a88) -> protocol `set_watchdog`. Reading returns the cached last value (11 under Yealink's UI).

Anti-clone check (`holtek_verify` 0x3c2c, called from `init_module` up to 3 times; module load fails if all fail): send random 16-bit x with cmd 0, wait 50 ms, read 16 bits with cmd 1, expect

    h = x >> 8, l = x & 0xFF
    hi = ((h << 3) + 0x75 + ((x >> 2) ^ (x >> 7))) & 0xFF
    lo = (((((h + l) & 0xFF) >> 2) ^ (((l - h) & 0xFF) << 2)) + 0x53) & 0xFF
    response = hi << 8 | lo

Whether the MCU needs this before it obeys other commands: **unknown** (probably not, it is a check on the MCU, not by it).

## LCD backlight (for reference)

`bklight_init` (0x81d0) for ids 35 and 65: control GPIO18 (`bklight_ctl`), then `bklight_init_pwm` (0x7fac): skip if GPIO35 is already muxed to MISC (U-Boot did it), else mux GPIO35 to function 13, BusIntfClock |= 0x10000, chipc LEDClock (0x8000004C) |= 0x20000, PWM register 0x80002040: |= 6, clear bits 3-7, |= 1. `bklight_set` (0x7898, case at 0x7d0c): level 0 drives GPIO18 high (off); otherwise PWM bits 3-7 = 30 - 3*ceil(level/10) (smaller is brighter) and GPIO18 low.

## Other GPIOs in keypad.ko (ids other than 37)

`misc_init` (0x9aec): GPIO31 `handfree_gpio`, output, set 1 at init, toggled by `misc_set_voice_hf` (**guess**: speaker amplifier enable). Headset detect and "mbs" GPIOs are -1 (absent).

## Touch: TSC2046 on the SoC SPI host

Source: `tsc.ko` functions `spi_std_*` (0x1428..0x1798), `tsc2046_*` (0x17f0..0x21d4), `iss_*` (0x9b4..0xb8c); data at .data 0x3c..0x128.

Bus (from `t4x_spi_board` .data 0x3c, `spi_std_init` 0x1650, `spi_get_iocs` 0x17b4, `t4x_spi_cs_control` 0x17c8):

- `spi_board_info`: modalias "tsc", bus 0, chip select 0, mode 0, max 2 MHz; 8-bit words.
- Chip select is software: GPIO25, output, idle high, low while selected.
- SPI bus 0 is Yealink's `drivers/spi/spi_bcmring.c` on the SPI host block **SPIH at 0x80015000, IRQ 26** (mainline `MM_ADDR_IO_SPIH`, `IRQ_SPIH`). Its register use (`SpiHw_Init` 0xC0039000, `SpiHw_SetBitRate` 0xC0038454, `SpiHw_ReadFifo` 0xC00382D0, probe at 0xC01BA9C8) is exactly an **ARM PL022 SSP**: CR0 +0x00 (DSS bits 3:0, FRF 5:4, SPO/SPH 7:6, SCR 15:8), CR1 +0x04 (SSE bit 1, MS bit 2), DR +0x08, SR +0x0C (TNF bit 1, RNE bit 2), CPSR +0x10 (even 2-254), IMSC +0x14. The probe also holds the PrimeCell id 0x00041022 with mask 0x000FFFFF (data at 0xC03B952C). So mainline `spi-pl022` should drive it.
- SPI clock: clk "SPI" set to 100 MHz (chipc SPIClock); bus clock gate bit 22.
- Pins: GPIO22, 23, 24 muxed to function 3 (SPI) by Yealink's board init (0xC001AA34, `gpiomux_requestGroup(16)`, group table at 0xC02EB808). Which pin is SCK, MOSI, MISO: **unknown**.

Controller and wiring (`t48_tsc_info` .data 0x100, getters at 0x10f4..0x11a8):

| Field | Value |
|---|---|
| pen IRQ GPIO | 26 (IRQ 126), falling edge, pen down = low |
| busy GPIO | 21, input (requested, never read) |
| reset GPIO | 16 (not used by the TSC2046 path) |
| debounce before sampling | 1 ms |
| delay between samples while down | 1 ms |
| poll delay | 200 us |

Protocol (`tsc2046_init` 0x214c, `tsc2046_coordinate` 0x185c, `tsc2046_process` 0x19cc):

- Init: send one byte 0x90 (measure Y, power-down mode 00 = PENIRQ enabled).
- Pen IRQ (`tsc2046_interrupt` 0x21d4): disable IRQ, queue work. Work sleeps 1 ms, samples, re-enables the IRQ when the pen is up.
- One coordinate = one 21-byte full-duplex transfer. TX: `D1 00` x10 then 00 for X (`tsc2046_cmd_x`, .data 0x80), `91 00` x10 then 00 for Y (`tsc2046_cmd_y`, .data 0x95). 0xD1/0x91 = start, X/Y channel, 12 bit, differential, PD=01. Sample i = (rx[2i+1] << 5) | (rx[2i+2] >> 3), ten samples, 16 clocks per conversion.
- Ten samples are partly sorted and four of them (indices 5-8) averaged; if their spread exceeds `use_max_min_jitter` (default 280) the result is 10000 = invalid.
- Two rounds of three X/Y pairs (2.5 ms apart, then 750 us). Per round the closest pair is averaged (spread must be under `use_sample_jitter`, default 70; pair difference at most 30 in X and 40 in Y). The two rounds must agree within 89 (X) and 119 (Y). Final X/Y = mean of the rounds.
- Reported only if it moved by at least `use_x_jitter` (12) or `use_y_jitter` (16). Pressure is a constant 60000 while down.
- Pen up (GPIO26 high after a touch): report the last X/Y with pressure 0 and BTN_TOUCH 0.

Values are **raw 12-bit ADC (0-4095), no calibration or rotation in the kernel**; Yealink's userspace does that. Axis direction versus screen: **unknown**.

## What the modules export to userspace (for reference)

keypad.ko:
- Input device name and phys "keypad", id 0x0101/0x0101, EV_KEY only, codes 0-255 enabled; events are key code + SYN (`input_dev_init` 0x3434, `input_dev_report` 0x232c).
- An input handler "keypad_handler" that gives the char device `/dev/keypad0` (read, write, poll, ioctl in `handler_*`, 0x2238..0x3360; ioctls not decoded).
- /proc names seen in `keypad_create_proc`: `keypad/` with watchdog, led, ver, dbg, reg, led_flick, lcd_bklight, lcd_bklight_pwm, spanspeed, hookscan, holtek_hw, holtek_sw, holtek_encrypt, holtek_msg, gpio, key_test, hook_sta; plus `compat/` (led, fsk, watchdog) and `pstn`.
- Useful for live tests under Yealink: `/proc/keypad/reg` takes `r <virt addr> 0` or `w <virt addr> <val>` and dereferences the virtual address directly (e.g. `echo r 0xf800d000 0`, result in dmesg). `/proc/keypad/holtek_msg` takes `R|S <cmd> <cmd bits> <data> <data bits>` and runs a raw Holtek read or write (32 data bits max, result in dmesg).

tsc.ko:
- Input device "YL tsc", phys "ts/input0", BUS_HOST; EV_SYN, EV_KEY (BTN_TOUCH), EV_ABS (ABS_X, ABS_Y, ABS_PRESSURE, all 0-4095) (`iss_init` 0xabc, `tsc2046_issfix` 0x17f0, `iss_report_single` 0x9b4).
- /proc/tsc/: ver, dbg, max_min_jitter, sample_jitter, x_jitter, y_jitter, tp_firmware_up, tp_firmware_ver.
- Module parameters: use_sample_jitter 70, use_x_jitter 12, use_y_jitter 16, use_max_min_jitter 280, use_reset_time 360, use_debug.

## Open questions that need the phone

1. Hardware id byte 6 (35 or 65) and Holtek protocol version: `/proc/keypad/holtek_hw`, `/proc/keypad/holtek_sw`, `/proc/cmdline`.
2. The keycode row offset: does digit 1 give 57 or 65?
3. Hook polarity: GPIO29 level on-hook and off-hook.
4. Which physical LEDs are LED word bits 0-3 (Holtek, GPIO3, GPIO4, GPIO5).
5. SPI pin roles on GPIO22/23/24 (only matters for a bit-banged fallback; the PL022 block handles it).
6. Touch axis orientation and Yealink's calibration constants.
7. Meaning of KEYC +0x18 and the CTRL bits 3 and 4, and whether clearing and setting CTRL bit 1 is the interrupt ack (a register dump of 0xF800D000-0xF800D01C under Yealink would show the live values).
