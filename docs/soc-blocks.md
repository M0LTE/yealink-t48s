# SoC blocks we do not drive: video decoder, graphics engine, crypto

Desk research for issue #64, 2026-11-24. Nothing here was run on a phone. Sources are Yealink's GPL kernel source (`gpl-src/dl/linux-kernel.zip`, paths below are inside it), Yealink's running kernel image (`dumps/vmlinux-image.bin`, loaded at 0xC0008000) with the phone's `/proc/kallsyms` (`dumps/audio/kallsyms.txt`), and the web. Things not known are marked **unknown**; inferences are marked **inferred**.

## Summary

| Block | Address, IRQ | On the BCM11188 | Register-level code | Verdict |
|---|---|---|---|---|
| Video decoder (VDEC) | 0x80700000, IRQ 56 | absent, per Broadcom's own table | none anywhere | not usable; a media player decodes in software |
| Graphics engine (GE v3) | 0x80300000, IRQ 29 | present, and Yealink's `lcd.ko` uses it | in Yealink's kernel binary with symbols; caller source is GPL | usable: fill, blit, alpha blend, colour conversion. No scaling, no rotation |
| RNG | 0x8000C000, IRQ 52 | present | trivial; same layout as mainline `bcm2835-rng` | worth doing, small |
| SPU (AES, DES, SHA-1, MD5) | 0x30700000 (data), 0x8000A000 (keys), IRQ 55 | present | in Yealink's kernel binary; no mainline driver fits | low value |
| PKA (RSA, DSA maths) | 0x8000B000 | present | driver binary in Yealink's kernel, no source | low value |

## Which blocks the BCM11188 has

The GPL source decides this at compile time, per chip, in `arch/arm/mach-bcmring/include/mach/csp/cap_inline.h`, and has no BCM11188 entry (it is built as BCM11107). Yealink's shipped kernel is newer: `cap_isPresent` (0xC0044DF8) is a real function that looks the chip up in a table of 11 chips at 0xC03A3FF8 (68 bytes each: chip id, one byte per capability, then channel counts and maximum clock speeds). `getChipMap` (0xC0044C38) picks the row from the chip id.

The BCM11188 row, decoded from the image:

| Capability | Value | Meaning |
|---|---|---|
| GE | 1 | present |
| VDEC | 0 | absent |
| SPU, PKA, RNG | 1, 1, 1 | present |
| CLCD, KEYPAD, LEDM, PIF, UARTB, VPM | 1 | present |
| USB | 2 | controller 1 only; controller 0 absent |
| ETH_PHY, ETH_GMII | 3, 3 | both ports |
| ETH_SGMII | 0 | absent |
| TSC | 0 | no on-chip touch controller (matches the board: touch is an external TSC2046) |
| SDIO | 0 | both absent |
| BBL (RTC, battery-backed RAM) | 0 | absent |
| EHSS | 1 | channel 0 only |
| APM | 3 | channels A and B; 2 APM channels, 1 PCM channel |
| Speeds | | ARM 300 MHz (nominal; the phone measures 499 BogoMIPS, so U-Boot runs it at 500 MHz), bus 100 MHz, DDR 200 MHz, VPM 200 MHz, LCD clock 206 MHz |

For comparison, VDEC is 1 only for the 11107, 11211, 11221 and 11189 (the 11189 row is a 500 MHz part with VDEC and TSC, **inferred** to be the video-phone sibling). The other chips in the table: 11109, 11170, 11110, 11195, 11198, 11199.

Caveats:

- This is Broadcom's software table, not a fuse readout. It matches what we know of the board (touch, USB port, two APM channels), so it is probably right.
- The chip id used for the lookup is `ChipId >> 12` from 0x80000000, unless the words at 0x4000FFF8 (top of VRAM) are the magic 0x0C1D2E3F followed by an override id. An unknown id falls back to the 11107 row (everything present). **Unknown**: what our phone's ChipId register reads. One read settles it.
- This changes two other lines of #64: SDIO and BBL are absent, and of the two USB controllers only controller 1 exists.

## VDEC: the video decoder

- **What it is**: a Hantro core, not VideoCore. `intcHw_reg.h` and `irqs.h` comment the interrupt as "Hantro Video Decoder interrupt" (INTC1 bit 24, IRQ 56). Hantro (later On2, Google, VeriSilicon) licensed hardwired decoders; they need no firmware blob, but the bitstream parsing lives in a closed userspace library, with only a thin kernel shim (`hx170dec` plus a `memalloc` buffer driver).
- **Which Hantro core, codecs, resolution**: **unknown**. No datasheet is public (Broadcom NDA only; a distributor page for the BCM11107 shows a "video decoder" box in the block diagram and nothing more). By the chip's date (2008 to 2009) an 8170 or 8190 class core is likely (**inferred**; those do H.264, MPEG-4, H.263, VC-1 and JPEG). Hantro cores carry an id in their first register, which would settle it on a chip that has one.
- **Code in the Yealink source**: only scaffolding. `CONFIG_VDEC` in `drivers/char/broadcom/Kconfig` reserves memory at boot (`include/linux/broadcom/vdec_cfg.h`, `bootmemheap.h`; 14 MB in the BCM5892 board file), `BCM_VDEC_MAJOR` 210 is allotted, and chipc has a bus clock gate (BusIntfClock bit 1) and a reset bit (soft reset bit 44). No driver, no register header, no library. Yealink's kernel image has the strings `bcm-vdec-memalloc` and `VdecHeapSize` but no decoder driver symbols.
- **Elsewhere**: nothing found. GitHub code search finds no bcmring Hantro driver. Mainline never carried one (mainline `mach-bcmring`, 2.6.32 to 3.6, has only the clock, DMA and timer support code). Mainline's `hantro-vpu` driver handles the later G1 lineage on other SoCs; whether its register layout fits this core is **unknown**.
- **Conclusion**: absent on the BCM11188 per Broadcom's table, undocumented, and dependent on a closed library even where present. Not a route to a media player on the T48S. Software decoding on the 500 MHz ARM1176 (no NEON) is the only option: audio is easy, video only at small sizes.

## GE: the 2D graphics engine

- **Present and in use.** The BCM11188 row has GE = 1, and Yealink's `lcd.ko` contains `ge_graph.c` (`ge_init`, `ge_device_ioctl`, `ge_device_mmap`, `ge_graph_isr`) and calls `gev3Hw_Init`, `gev3Hw_Exit` and `gev3Hw_clearInterrupt` in the kernel (module symbols in `kallsyms.txt`). **Unknown**: whether Yealink's UI really draws through it, and the ioctl format of Yealink's device. We do not have `lcd.ko` in `dumps/`.
- **What it does** (public API in `include/linux/broadcom/bcmring_ge.h`, driver in `drivers/char/bcmring/bcmring_gev3.c`, both GPL):
  - Colour fill.
  - Blit with any of the 16 raster operations of two sources, optional colour-key transparency, optional fill colour in place of source 1.
  - Alpha blend: per-pixel alpha, global alpha (1 to 255), premultiplied sources, five choices for the destination alpha byte, dither, rounding.
  - Colour conversion as part of a blit or blend: source 1 may differ in format from the destination. Formats: RGB565, packed RGB888, xRGB8888, ARGB8888; and as blend sources only, YUV 4:2:2 interleaved, YUV 4:2:0 planar and semi-planar. So YUV video frames can be converted to RGB by the hardware (**inferred** from the header, untested).
  - A 16-entry palette and foreground/background colour registers (**inferred**: expansion of low-depth sources such as fonts).
  - Little-endian, big-endian and a "WinCE" byte order.
- **What it does not do**: no scaling, no rotation, no lines or polygons. One width and height serve source and destination. Maximum 1024 x 1024. Overlapping copies only work in ascending address order (Broadcom's driver bounces through a temporary buffer otherwise). Source 2 and destination must share a format.
- **How Broadcom drives it**: buffers are physical addresses of contiguous memory; one operation at a time; completion by interrupt 29 or by polling the status register; a queue of up to 64 requests in a kernel thread. Userspace interface is `/dev` major 203 with `GE_IOCTL_ENGAGE`.
- **Register-level information**: the layer below the driver (`csp/gev3Hw.h`, `gev3Hw.c`) is missing from Yealink's source drop and from every public tree found. TomTom's BCM4760 kernel uses the same `gev3Hw_*` calls for its LCD, again without the file. But the compiled functions are in Yealink's kernel with their names, the same position we were in for audio:

  | Function | Address |
  |---|---|
  | `gev3Hw_alphablend` | 0xC00368A4 |
  | `gev3Hw_rasterOperation` | 0xC0036EA4 |
  | `gev3Hw_colorFill` | 0xC00373C4 |
  | `gev3Hw_Reset`, `_PowerOn`, `_clearInterrupt`, `_Init`, `_Exit` | 0xC00375F8 to 0xC0037750 |
  | `gev3Hw_isComplete`, `_setColorPalette`, `_getColorPalette` | 0xC0036880, 0xC00367C0, 0xC0036830 |
  | older `geHw_*` (GE v2) set | 0xC0035DF4 to 0xC003638C |

  `tools/regsum.py dumps/vmlinux-image.bin dumps/audio/kallsyms.txt c00367c0 c00377dc` lists every register access. The parameter structs are known from the GPL caller (`geHw_ALPHABLENDING_PAR_t`, `geHw_ROP_TRANSPARENT_PAR_t`, `geHw_COLORFILLING_PAR_t` field names appear in `bcmring_gev3.c`), so each bit can be tied to a named option.
- **Register names**, from the debug print strings in the image, matched by order to the offsets the v2 print function reads (**inferred** pairing): GESR 0x00 (status), GECR 0x04 (control), GEER 0x08 (engine start), GECER 0x0C (clock enable), GEOPR 0x10 (operation), GEABR 0x14 (alpha blend), GESSAR 0x20, GESCAR 0x24, GEDBAR 0x28, GEDCAR 0x2C (source and destination addresses), GECDR 0x30 (formats), GESPR 0x34, GEDPR 0x38 (pitches), GESOR 0x3C, GEWR 0x40, GEHR 0x44 (width, height), GE2SBA1R 0x50, GE2SBA2R 0x54, GE2DSA1R 0x58, GE2DSA2R 0x5C (chroma plane addresses), 0x60 to 0x6C debug state, GEBGR 0x80, GEFGR 0x84 (background, foreground), GEPLR0 to 15 at 0x100 to 0x13C (palette). The v3 code also uses 0x48 and 0xFC.
- **Bring-up, from the disassembly**: `gev3Hw_Init(1)` sets chipc MiscCtrl bit 18 (0x80000090, "select GE2/GE3" in `chipcHw_reg.h`), then `gev3Hw_PowerOn(1)` writes 1 to 0x8030000C. An operation is started by writing 1 to 0x80300008. Done is bit 15 of 0x80300000; the interrupt is cleared by setting bit 8 there. Reset sets bit 31 of 0x80300008; release clears it and sets bit 19 of 0x803000FC. Chipc also has a bus clock gate (BusIntfClock bit 9) and a reset bit (soft reset bit 43) for the block.
- **No firmware blob.** It is a fixed-function engine.
- **Conclusion**: usable, and the best find here. A kernel driver is a port of `bcmring_gev3.c` on top of the recovered `gev3Hw` functions, roughly the size of the audio hardware layer. It gives the shell, games and the demo fast fills, sprite blits with colour key or alpha, and whole-screen copies. Anything scaled or rotated stays in software.

## Crypto: RNG, SPU, PKA

- **RNG** (0x8000C000). Yealink's kernel registers it as a hwrng (`bcmring-rng`, `rng_probe` at 0xC00186E0, `rng_data_read` at 0xC01747A0). Registers from the disassembly: control at +0x00, status at +0x04 (warm-up count written as 0xFF000; available word count read back), data at +0x08, interrupt mask at +0x10 (written 1). That is the layout of mainline `drivers/char/hw_random/bcm2835-rng.c` (**inferred** match, same Broadcom core; to be confirmed against the bit values). Soft reset bit 24. On Linux 6.18 this should be a device tree node; on 3.6 a 50-line driver. Feeds SSH and TLS key generation.
- **SPU** (data path 0x30700000, key table 0x8000A000, IRQ 55). AES, DES, SHA-1, MD5 with DMA in and out. Yealink's kernel has `spuHw_*` (0xC003FA84 to 0xC0040350) and an OCF driver (`ocf_bcmring`, `spu_dma_*`); the source drop has only `include/linux/broadcom/bcm_spu.h` and the DMA device entries. Mainline `drivers/crypto/bcm` drives later SPU cores through a mailbox and does not fit as it stands; its message format headers may match (**unknown**). Chipc has an OTP bit "limited SPU throughput". Low value: software AES on this CPU is enough for a phone.
- **PKA** (0x8000B000). `pka_driver` in Yealink's kernel (plugin interface in `include/linux/broadcom/pka_register.h`), no hardware source, no mainline driver. Low value.

## Telling at runtime what the chip has

Chip control (chipc) is at 0x80000000 and is always clocked. Layout from `arch/arm/mach-bcmring/include/mach/csp/chipcHw_reg.h`; offsets counted from the struct and checked against two we already use (BusIntfClock 0x5C, GpioMux 0x118).

| Register | Address | Use |
|---|---|---|
| ChipId | 0x80000000 | bits 31:12 product (expect 0x11188), bits 11:0 revision |
| BusIntfClock | 0x8000005C | bus clock gates: VDEC bit 1, GE bit 9, OTP bit 17, SPU bit 19 |
| SoftReset1, SoftReset2 | 0x80000080, 0x80000084 | 64 reset bits; RNG 24, PKA 23, SPU 25, OTP 42, GE 43, VDEC 44 |
| MiscCtrl | 0x80000090 | bit 18 selects GE v2 or v3 |
| SoftOTP1, SoftOTP2 | 0x800000F4, 0x800000F8 | "software OTP control": one disable bit per block |
| SoftStraps, PinStraps | 0x800000FC, 0x80000100 | boot straps |

SoftOTP bits (64-bit value, SoftOTP1 is the low word), from the `chipcHw_REG_OTP_*` defines: VPM 0, Ethernet PHY 1 to 2, GMII 3 to 4, SGMII 5 to 6, USB 7 to 8, touch 9, HSS 10 to 11, SDIO 12 to 13, UART 14, keypad 15, LCD 16, **GE 17**, LED 18, BBL 19, **VDEC 20**, PIF 21, APM 24 to 28, AXI speed 29 to 30, VPM speed 31 to 32, LCD speed 33 to 34, SPU slow 40, security off 41. A set bit disables. The low bits are the same as the `CAP_CONFIG0_*_DIS` bits in `cap_inline.h`.

- **Unknown**: whether these two registers read back the real fuses or only what software has disabled since reset. If they read 0 the answer is the latter. Broadcom's code only ever ORs bits into them, so **never write them**: a write can switch a block off until the next reset, or for all we know for good.
- The fuses themselves are in the OTP block at 0x8000F000 (144 rows of 32 bits; `otpHw_readRawData` at 0xC00419A8, command registers at +0x04 to +0x14). The same block has write and lock commands. Reading it adds little over the above and a slip could burn a fuse, so leave it alone.

### What an access to a missing block does

**Unknown** for a fused-off block. What we have seen on this chip: a block with its bus clock gated or held in reset answers with an imprecise external abort (fault 22), which `t48s_audio` already catches with a fault hook (`kernel6/drivers/t48s_audio.c`, `t48s_abort`); under Yealink's kernel `devmem` on some offsets dies with a bus error and the phone carries on (`docs/audio.md`). No hang has been seen yet, but one cannot be ruled out, hence one block per boot with a watchdog or a hand on the power.

### Probe plan, safest first

1. **Chipc, read only, single words** (not ranges: some chipc offsets fault): ChipId, SoftOTP1, SoftOTP2, SoftStraps, PinStraps, BusIntfClock, SoftReset1, SoftReset2, MiscCtrl. Plain `devmem` reads on our kernel. Compare ChipId with 0x11188 and SoftOTP with the table above (expected set if they mirror fuses: SGMII, USB 0, touch, HSS 1, SDIO, BBL, VDEC).
2. **GE** after a kexec from Yealink's firmware, where `lcd.ko` has already powered it: with the abort hook armed, read 0x80300000 and 0x8030000C. A sane status and clock-enable 1 confirm the block. Then from a cold boot: set BusIntfClock bit 9, clear the GE reset, do the `gev3Hw_Init` and `PowerOn` writes, and run one colour fill into an off-screen buffer, polling bit 15 of the status.
3. **RNG**, same boot or the next: with the hook armed read 0x8000C004; nonzero word count and changing data at +0x08 confirm it.
4. **SPU, PKA**: only if someone wants them. Read one register each with the hook armed, one per boot.
5. **VDEC**: expected absent, nothing to gain. If wanted for completeness, last, alone, with the hook armed: set BusIntfClock bit 1, read 0x80700000. An abort or zero confirms absence; an id like 0x8170xxxx or 0x8190xxxx would overturn the table.

Never: writes to SoftOTP1 or SoftOTP2, any write to 0x8000F000 to 0x8000F014, block reads across chipc.

## Sources

- Yealink GPL kernel, `gpl-src/dl/linux-kernel.zip`: `arch/arm/mach-bcmring/include/mach/csp/{cap.h,cap_inline.h,mm_addr.h,chipcHw_reg.h,intcHw_reg.h}`, `arch/arm/mach-bcmring/include/mach/irqs.h`, `drivers/char/bcmring/bcmring_gev3.c`, `include/linux/broadcom/{bcmring_ge.h,vdec_cfg.h,bcm_major.h,bcm_spu.h,pka_register.h,otp.h}`, `drivers/char/broadcom/{otp.c,Kconfig}`.
- Yealink kernel image and symbols: `dumps/vmlinux-image.bin`, `dumps/audio/kallsyms.txt`; disassembly with `arm-linux-gnueabihf-objdump -D -b binary -marm --adjust-vma=0xc0008000`; register summaries with `tools/regsum.py`.
- Same source tree on GitHub: `shuishang/kernel_bcm5892`. TomTom's use of the same engine: `kzlin129/tt-gpl`, `go12/linux-2.6.28.10/drivers/video/bcm4760_clcd.c`.
- BCM11107 documentation: NDA only (Digi-Key forum, https://forum.digikey.com/t/bcm11107kfbg-datasheet/61262). No product brief for the BCM11107 or BCM11188 with video or graphics detail was found.
