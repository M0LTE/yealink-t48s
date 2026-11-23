# Audio: how the phone gets sound in and out, and a plan for our own driver

Research for issue #6, done 2026-11-22/23 at night with a hard rule of making no sound: nothing was played, no gain, mux, amplifier or GPIO was written, only registers read and microphones recorded. Statements are marked **measured** (seen on this phone), **code** (read from Yealink's binaries) or **guess**.

## Summary

- **The voice DSP is not needed to get PCM in or out.** The audio converters (ADC and DAC) live in the SoC's APM block, and the ARM moves samples to and from them itself, over DMA. That is how Yealink's own firmware works: its `halaudio_apm.ko` module runs four DMA channels between RAM and the APM FIFOs, and the DSP only sees the samples afterwards, handed over by a software mixer on the ARM.
- **Proved for capture, silently.** Under our own kernel, a 200-line user-space program (`native/apmcap.c`) recorded the microphone at exactly 16 kHz straight from the APM block, with Yealink's kernel and modules gone and the DSP halted. No vendor code involved.
- **The register-level code is recoverable.** Broadcom's `ApmHw_*` functions (sample rate, gains, input select, amplifier power, sidetone, equaliser) are built into Yealink's kernel image with their names in `/proc/kallsyms`. They are short and mostly constant register writes. Broadcom's GPL driver that calls them is public, so the call order is known too.
- **What the DSP does** is the telephony processing: G.711/G.722/G.729 codecs, echo cancellation, tones, jitter buffer. It runs a 1.9 MB proprietary image. We would replace all of that with ordinary software on the ARM (a SIP stack's own codecs, speexdsp or WebRTC echo cancelling), not try to drive the blob.
- **Still open:** which of the four analog inputs and four outputs is the handset, the headset and the speakerphone. The register state gives strong hints but two readings are possible, and telling them apart needs someone to make a noise. That is daytime test 1 below, about five minutes.

Recommended path: write a small ALSA driver for our 3.6 kernel that drives the APM codecs over DMA controller 0 directly, capture first (testable in silence), then playback and path switching with Tom listening.

## How Yealink's audio is put together

**Measured** from `/proc/halaudio`, `/proc/amxr`, `/proc/bcmapm/*`, `/proc/dma/*`, `/proc/interrupts` and `lsmod` on firmware 66.82.0.31 at idle:

```
 handset / headset / speaker / mics
            |  analog
   APM block (0x30600000): two codecs, "A" (CH0) and "B" (CH1), each one ADC + one DAC
            |  FIFOs, 16-bit samples, 16 kHz
   DMA controller 0 (0x30200000), channels 2-5, 5 ms blocks (160 bytes)
            |
   halaudio_apm.ko  (ARM, GPL v2, Broadcom "HAL Audio BCMRING APM Driver", Yealink build 1.4.0.8)
            |  ports halaudio.apm0, halaudio.apm1
   amxr.ko  (ARM software mixer; at idle apm1 <-> ept.bind1)
            |
   ept_driver.ko (endpoint: RTP, jitter buffer, codec control) <-> pxcLdxHausware.ko <-> voice DSP (VPM)
            |
   ipvpserver (user space, opens /dev/halaudio, /dev/amxr, /dev/ept0, /dev/ldx_hsw-0)
```

| Module | Size | Declared licence | Role |
|---|---|---|---|
| `amxr.ko` | 27 KB | Proprietary | software audio mixer between "ports" |
| `halaudio.ko` | 20 KB | Proprietary | framework: interfaces, codecs, gain blocks, power |
| `halaudio_apm.ko` | 132 KB | GPL v2 | the APM codec driver (plus Yealink's I2S, Bluetooth and "pcm_hook" extras) |
| `pxcLdxSupport.ko` | 55 KB | Proprietary | DSP loader support |
| `pxcLdxHausware.ko` | 242 KB | GPL | DSP loader and host side of the DSP's "hausware" framework |
| `ept_driver.ko` | 357 KB | GPL | telephony endpoint |

They are in `/boot/driver/` on the phone, loaded by `/phone/scripts/VpmDriver.sh`; all are unstripped. Copies are in `dumps/audio/` (not committed: vendor code).

DMA channels while idle (**measured**, read-only):

| DMA0 channel | Device | Direction | APM FIFO | CTL low | CFG high | IRQ |
|---|---|---|---|---|---|---|
| 2 | `apm_a_tx` | RAM to codec A DAC | 0x30680000 | 0x00904835 | 0x1806 (dest request line 3) | 2 |
| 3 | `apm_a_rx` | codec A ADC to RAM | 0x30670000 | 0x1a204827 | 0x0106 (source request line 2) | 3 |
| 4 | `apm_b_tx` | RAM to codec B DAC | 0x306a0000 | 0x00904835 | 0x2806 (dest request line 5) | 4 |
| 5 | `apm_b_rx` | codec B ADC to RAM | 0x30690000 | 0x1a204827 | 0x0206 (source request line 4) | 5 |

CFG low is 0xc0 on all four. Each block is 40 words (160 bytes, 80 samples, 5 ms); the channels run linked lists of two blocks that the driver keeps re-queuing. Note that `/proc/bcmapm/apm0` labels the FIFO addresses the other way round ("DMA Output ... fifo=0x30670000"); the DMA registers are the truth, and the capture experiment confirms 0x30670000 and 0x30690000 are the ADC sides. Request line numbers match mainline's `arch/arm/mach-bcmring/dma_device.c` (`DMA_DEVICE_APM_CODEC_A_DEV_TO_MEM` and friends), which our 3.6 tree still has.

The "apm" interrupt (IRQ 28) is for FIFO overflow and underflow errors only; it never fired in normal running.

## Question 1: PCM without the DSP. Yes.

Evidence, strongest first:

1. **Measured under our kernel.** After a kexec boot, Yealink's kernel and every module are gone and all DMA channels are stopped (the loader does that). `apmcap` then starts DMA0 channel 5 itself, with the control words from the table above (interrupts off), over a ring of blocks in RAM the kernel does not own, and polls it:

   ```
   t48s# apmcap -c 1 -s 4 -H
   GPIO bank0 data 6f7bffff dir 920c4c38: GPIO31 (speaker amp) is output, level 0
   APM ctl[0c]=f0810204 irqstat[04]=000000f0 irqen[08]=000000f0 chA[20]=0c887f7f chB[50]=8c887f7f
   DSP halted: chipc[10]=00103d06 vpm halt[301c]=1
   codec B ADC (apm_b_rx): 64000 samples in 4 s asked; APM irqstat[04]=000000f0
   min -80 max 64 mean -5.0 rms 15.0 (-66.8 dBFS) zeros 1588 unwritten 0
   real    0m 4.05s
   ```

   64000 samples in 4.05 s is 16 kHz to within the polling grain, so the codec is free-running on its own clock and delivering through DMA with nothing but our code attached. The level (rms 15 counts, a quiet room at night) is what Yealink's own `/proc/bcmapm/apm1` showed for the same input before the jump (values within +-0x22). `-H` applies the same register write as Yealink's `vpmHw_Halt` first. Codec A's ADC delivered exact zeros, as expected: Yealink leaves its digital microphone gain muted at idle (register 0x40 low half is 0).
2. **Code.** `halaudio_apm.ko` imports `dma_alloc_channel`, `dma_transfer`, `dma_set_device_handler` and the `ApmHw_*` functions and nothing from the DSP side. The DSP modules load after it and attach through the mixer.
3. **Measured under Yealink.** At idle the DSP interrupt counter (`vpmIsr`) stands still while the four APM DMA interrupts tick at 200 per second each.

What this does not prove yet: playback (same mechanism in the other direction, not tested because it would make sound), and bringing the APM block up from cold. Our capture relied on Yealink having initialised the block before the kexec jump. The cold initialisation is known from code (below) but untested.

## Question 2: the DSP, for the record

- **Firmware:** `/phone/bin/dspapp1107_lcc.bin` (1,916,336 bytes, sha256 86b644dd8c707510...), reached through `/lib/firmware/` by `request_firmware` from `pxcLdxHausware.ko`. Format: four records of {load address, byte count, data}: 624 bytes at 0x30800000 (VPM program memory, the boot vector), 1,000,640 bytes at 0x00010000, 65,536 bytes at 0x30A00000 (VPM data memory), 849,504 bytes at 0x001222B0. Yealink's boot log prints the same four ("VPM Load address=...").
- **Memory:** the two big records go into ordinary RAM below 4 MB. That is why Yealink's kernel starts its own RAM at 0x00400000: the first 4 MB belong to the DSP. (Mainline also names 16 MB at 0x0F000000 as "reserved external memory for VPM"; this firmware does not load anything there.)
- **Start-up (code, `vpmHw_*` in Yealink's kernel, same order as Broadcom's GPL `bcmring_vpm.c`):** `vpmHw_InitandHalt` (set halt at 0x8000301C, enable the VPM clock bit 16 in chipc 0x80000010, phase-align the clock, pulse two reset bits in chipc 0x80000080/84), copy the records in, `vpmHw_Run` (clear the halt). The ARM then talks to it through shared memory and IRQ 16.
- **Could our kernel load it?** Loading is easy. Driving it is not: the host side is `ept_driver.ko` plus `pxcLdxHausware.ko` plus `pxcLdxSupport.ko`, about 650 KB of code built for 2.6.27 with a proprietary message protocol, and `ipvpserver` on top. There is no reason to do it, because nothing on the path to the transducers needs it.
- **Licence notes (Tom decides):** the DSP image is Broadcom proprietary and may not be redistributed by us; an image of ours could at most load it from the owner's own flash. `amxr.ko`, `halaudio.ko` and `pxcLdxSupport.ko` declare themselves proprietary. `halaudio_apm.ko`, `ept_driver.ko` and `pxcLdxHausware.ko` declare GPL, and the `ApmHw_*` and `vpmHw_*` code is linked into the kernel image itself, so Yealink owes source for those on request. Their GPL download page only carries an old MIPS 2.6.10 kernel for other models (that is what `gpl-src/dl/linux-kernel.zip` is), not this phone's 2.6.27.47. Asking them for it is an option. The plan below needs none of the vendor binaries at run time.

## Question 3: codecs, paths, gains

### What is certain

Two codecs are in use (`/proc/halaudio`: "APM: codecs=2"); a third ADC-only channel "C" exists in the register map and is left unused. Each codec has one ADC with a two-way analog input switch (inputs "A" and "B") and one DAC with two analog outputs ("A" and "B"). So there are four microphone inputs and four outputs for three transducer pairs.

Register map of the APM block (physical 0x30600000 + offset), from the `ApmHw_*` code in Yealink's kernel (`tools/regsum.py` lists every access per function) and the live values at idle (**measured**; the same before and after the kexec jump):

| Offset | Idle value | What it is |
|---|---|---|
| 0x000 | 0 | FIFO flush strobe (`ApmHw_ChEnable` writes 0xa, 0xc or 0xf) |
| 0x004 | 0x0f running, 0xf0 once DMA stops | interrupt status, write 1 to clear; bits 4-7 are the FIFO error flags |
| 0x008 | 0xf0 | interrupt enable |
| 0x00c | 0xf0810204 | global control; top four bits are the enables: 0x5 codec A, 0xa codec B |
| 0x020 / 0x050 | 0x0c887f7f / 0x8c887f7f | codec A / B control; bit 31 held set while disabled |
| 0x024 / 0x054 | 0 / 0x8000 | sidetone (`ApmHW_SidetoneEnable`, `SidetoneVolumeSet`) |
| 0x02c / 0x05c | 0x00330000 / 0x00345a67 | DAC digital gain in the low 16 bits (A is muted) |
| 0x040 / 0x070 | 0x03130000 / 0x031329f1 | ADC digital gain in the low 16 bits (A is muted: exact zeros) |
| 0x044 / 0x074, 0x080, 0x084 | | sample-rate dividers and their load strobes (`ApmHw_SampleFreqSet`) |
| 0x120 | 0x40 | analog power-down bits (`ApmHw_AmpSetPowerDown`: bits 2-3, 5-6) and microphone bias (bits 1 and 4) |
| 0x124 / 0x12c | 0 / 0x02780000 | ADC analog front end of codec A / B: bit 25 selects input B, the preamp gain sits below it (`ApmHW_SetTxMux`, `ApmHw_PGAGainSet`, 0 to 42 dB in 3 dB steps) |
| 0x134 | 0x00053333 | output mute bits: 16 = B/out A, 17 = B/out B, 18 = A/out A, 19 = A/out B |
| 0x138 | 0xfdff3000 | output mode, two bits each (3 = off): bits 12-13 B/out A, 14-15 B/out B, 16-17 A/out A, 18-19 A/out B; output gains in the low bits |
| 0x200-0x284 | | codec C (ADC only) |
| 0x420-0x438 / 0x520-0x538 | 4, 4, 1, 1, 4 | FIFO thresholds and DMA request enables, codec A / B |

FIFOs are separate 64 KB windows: 0x30670000 (A ADC), 0x30680000 (A DAC), 0x30690000 (B ADC), 0x306A0000 (B DAC).

GPIO31 is an external amplifier enable (**code**, `apmAnaGainSet` in `halaudio_apm.ko`, where a string calls it `extern_amp_gpio`): it is driven high when codec A's output A is unmuted and low when it is muted. `keypad.ko` calls the same pin `handfree_gpio`. **Measured:** it is an output at level 0 at idle under Yealink and under our kernel, so the external amplifier is off.

The clocks are `APM200` (200 MHz from PLL2) and `APM100`, both shown enabled in `/proc/clk/listClocks`, plus the "audio channel enable" register in chipc (mainline `chipcHw_audioChannelEnable`, bits for channels A, B, C and the APM and NTP clocks).

### What is likely but not proven

At idle Yealink keeps exactly one path alive: codec B, input B, preamp near maximum, ADC and DAC gains set, output B on and unmuted, mixer connected to the endpoint. Everything on codec A is muted.

- **Reading 1 (guess, about 60%):** this is the speakerphone, kept ready. It matches a Snom D375 on the same chip family with the same Broadcom modules, where input B of codec B was measured as the base microphone, codec A input A as the handset microphone and codec A input B as the headset jack (see "Other people's work"). The loudspeaker would then be codec B output B.
- **Reading 2 (guess, about 40%):** this is the handset path, the default device. In its favour: sidetone is switched on for codec B (register 0x54), which is a handset or headset feature and makes no sense for a speakerphone, and GPIO31, named "handfree" by Yealink, follows codec A output A, which would make codec A the speakerphone codec.

Daytime test 1 settles it and also gives Yealink's gain settings for every mode.

## Question 4: plan for our own driver

An ALSA driver (`kernel/drivers/t48s_audio.c`), not a private character device: PJSIP and baresip both speak ALSA, and so do `arecord`/`aplay` for testing. It needs `CONFIG_SOUND`/`CONFIG_SND` turned on (they are off in our config) and programs DMA controller 0 directly, the way `t48s_eth.c` does with controller 1 (our kernel deliberately leaves controller 0 alone).

| Step | What | Size | Test | Needs Tom awake? |
|---|---|---|---|---|
| 0 | User-space capture proof (`native/apmcap.c`) | done | done | no |
| 1 | ALSA capture: one card, two capture devices (codec A, codec B), 16 kHz, 16-bit mono, 5 ms periods, DMA0 channels 3 and 5 with their block interrupts (IRQ 3, 5), error interrupt (IRQ 28). Leaves the analog setup as Yealink left it. | 1 to 2 days | `arecord` from codec B, compare with `apmcap` | no |
| 2 | Capture controls: input select, preamp gain, ADC digital gain, microphone bias, as ALSA mixer controls. Port of five short `ApmHw_*` functions. | 1 day | record each input | mostly no, see note |
| 3 | Playback: two playback devices on DMA0 channels 2 and 4, DAC digital gain, output mode and mute, amplifier power-down, GPIO31, ramp-down on stop (`ApmHw_AmpDacRampDown`) to avoid clicks. | 1 to 2 days | tone to each output at low level | **yes** |
| 4 | Cold start, so the live image (#7) works without Yealink having run first: clocks, chipc audio enable, APM soft reset, the `ApmHw_init` table (about 70 constant writes, already extracted), sample rate, FIFO setup. | 1 to 2 days | boot through U-Boot recovery, record and play | **yes** (possible click) |
| 5 | Paths as named ALSA controls ("Handset", "Headset", "Speaker"), sidetone for handset and headset, 8 kHz as a second rate. | 1 day | switch during a call | **yes** |
| 6 | In user space, with the SIP stack (#12): G.711/G.722 on the ARM, echo cancelling for the speakerphone (speexdsp is the first thing to try on this 500 MHz ARM11), ringer through the loudspeaker. | with #12 | calls | **yes** |

Note on step 2: capture-side writes are silent unless sidetone is on and an output is live, because sidetone feeds the microphone into the earpiece. The driver should switch sidetone off and mute the outputs before touching input settings in a night-time test; I did not try it tonight because I could not prove every output stage was off.

Risks: the equaliser coefficients (`ApmHw_WriteCoeff`) are loaded by Yealink at start-up and will be lost on a cold start; flat is fine to begin with. The speakerphone will need real echo-cancelling work. Codec C and the I2S/PCM ports are out of scope.

## Daytime tests for Tom

1. **Which path is which (5 minutes, makes dial tone).** Under Yealink (`sh tools/ourkernel.sh back`), run `sh tools/audio-modes.sh idle`, then lift the handset and run it with `handset`, put it back and press Speaker (`speaker`), press Headset (`headset`). It only reads registers. The result lands in `dumps/audio/modes.txt` and fills in the table above, with Yealink's gains per mode.
2. **Hear the capture (2 minutes, silent on the phone).** Under our kernel: `apmcap -c 1 -s 5 -o /tmp/m.raw` while talking near the base, then near the handset. Copy the file off and play it (`aplay -f S16_LE -r 16000`). `apmcap` is not in the kernel image yet; `make -C native apmcap` and scp it to `/tmp`.
3. **Playback**, when step 3 exists: a quiet 1 kHz tone to each output in turn, saying which transducer sounds.
4. **Cold start**, when step 4 exists: boot the live image from power-off and repeat 2 and 3.

## Things that bit me

- **Do not `cat /proc/ept/*` on Yealink's firmware.** After one pass that read every file there (together with the audio files below), Yealink's kernel had corrupted memory: oopses in the directory cache a few minutes later (a pointer overwritten with the text "stat"), `ps` segfaulting, `scp` failing. A reboot cleared it. The `ept` files are the likely cause (long text through the old one-page proc interface) but I did not go back to prove it. Read again after the reboot with no trouble: `/proc/halaudio`, `/proc/amxr`, `/proc/bcmapm/apm0`, `apm1` and `/proc/interrupts`.
- `cat /proc/bcmapm/apm_dump_mic0` (and `_spk`) blocks for ever waiting for a dump to be enabled; kill it by PID.
- `devmem` under Yealink gets a bus error past the last channel register of DMA0 (around 0x3020028C) and on some chipc and GPIO offsets; harmless (the process dies), but read in small ranges.
- busybox 1.10 on the phone has no `tail`, `tr`, `head -N` or `grep -A`.

## Other people's work

- **Broadcom's GPL driver for this exact block:** `drivers/char/broadcom/halaudio_drivers/bcmring/apm/apm_drv.c` (2928 lines, "Copyright 2008-2009 Broadcom", GPL v2) in <https://github.com/shuishang/kernel_bcm5892>, with the halaudio core, `amxr_drv.c`, the headers under `include/linux/broadcom/` and `drivers/char/bcmring/bcmring_vpm.c` (the DSP loader). It gives the call order (clocks, soft reset, `ApmHw_init`, sample rate, DMA, enable) and the DMA handling. It does not include `apmHw.c` or `apmHw_reg.h`: the register names and offsets are not public anywhere we could find, which is why they were taken from Yealink's kernel image instead.
- **halaudio and amxr cores** also appear in Samsung's i9082 kernel: <https://github.com/pawitp/android_kernel_samsung_i9082> (`drivers/char/broadcom/halaudio/`).
- **Snom D375 reverse engineering** (same SoC family, same 2.6.27.47 kernel and module set): <https://github.com/devilAPI/snom-telephone-cfw-reverse-engineering>, `docs/intercom.md`, `docs/sound.md`, `docs/speaker.md`. It uses the vendor modules through their ioctls rather than replacing them, and documents the halaudio gain blocks and which input is which microphone on that phone.
- **Not found:** a datasheet for the BCM11107/BCM11188 (NDA only), any GPL tarball from another phone maker with the bcmring audio code, or audio code in the 2009 mainline bcmring submission (it only had the DMA, timer and chip-control support code).

## Files

- `native/apmcap.c`: the capture proof. Records only.
- `tools/regsum.py`: lists the register accesses of each function in Yealink's kernel image.
- `tools/audio-modes.sh`: daytime test 1.
- `dumps/audio/` (not committed): the six modules, the DSP image, `kallsyms.txt`, the idle register dump, the `ApmHw_*` and `vpmHw_*` register summaries (`apmhw.sum`, `vpmhw.sum`), the `/proc` captures and the oops log.
