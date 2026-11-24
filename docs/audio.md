# Audio: how the phone gets sound in and out, and a plan for our own driver

Research for issue #6, done 2026-11-22/23 at night with a hard rule of making no sound: nothing was played, no gain, mux, amplifier or GPIO was written, only registers read and microphones recorded. Statements are marked **measured** (seen on this phone), **code** (read from Yealink's binaries) or **guess**.

## Summary

- **The voice DSP is not needed to get PCM in or out.** The audio converters (ADC and DAC) live in the SoC's APM block, and the ARM moves samples to and from them itself, over DMA. That is how Yealink's own firmware works: its `halaudio_apm.ko` module runs four DMA channels between RAM and the APM FIFOs, and the DSP only sees the samples afterwards, handed over by a software mixer on the ARM.
- **Proved for capture, silently.** Under our own kernel, a 200-line user-space program (`native/apmcap.c`) recorded the microphone at exactly 16 kHz straight from the APM block, with Yealink's kernel and modules gone and the DSP halted. No vendor code involved.
- **The register-level code is recoverable.** Broadcom's `ApmHw_*` functions (sample rate, gains, input select, amplifier power, sidetone, equaliser) are built into Yealink's kernel image with their names in `/proc/kallsyms`. They are short and mostly constant register writes. Broadcom's GPL driver that calls them is public, so the call order is known too.
- **What the DSP does** is the telephony processing: G.711/G.722/G.729 codecs, echo cancellation, tones, jitter buffer. It runs a 1.9 MB proprietary image. We would replace all of that with ordinary software on the ARM (a SIP stack's own codecs, speexdsp or WebRTC echo cancelling), not try to drive the blob.
- **Still open:** which of the four analog inputs and four outputs is the handset, the headset and the speakerphone. The register state gives strong hints but two readings are possible, and telling them apart needs someone to make a noise. That is daytime test 1 below, about five minutes.

Recommended path: write a small ALSA driver for our 3.6 kernel that drives the APM codecs over DMA controller 0 directly, capture first (testable in silence), then playback and path switching with Tom listening.

**Update, night of 2026-11-23 (issue #24): the capture half of that driver exists and works.** `kernel/drivers/t48s_audio.c` gives an ALSA card with one capture device per codec and mixer controls for the capture side. See "The capture driver" below.

**Update, daytime 2026-11-23: playback works too.** Both codecs play, the outputs, gains, sidetone and loudspeaker amplifier are mixer controls, and the driver starts with everything off. Measured with the phone's own microphones as the listener; see "Playback" at the end.

**Update, 2026-11-24: cold start.** The driver starts the audio block itself when Yealink's firmware has not run, on both kernels, and can no longer stop the boot. See "Cold start" at the end.

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
| 0x004 | 0x0f running, 0xf0 once DMA stops | interrupt status, write 1 to clear; FIFO error flags: bit 4 DAC A underflow, 5 DAC B underflow, 6 ADC A overflow, 7 ADC B overflow (bits 6 and 7 **measured**: each stays clear while its capture runs) |
| 0x008 | 0xf0 | interrupt enable |
| 0x00c | 0xf0810204 | global control; top four bits are the enables: 0x5 codec A, 0xa codec B |
| 0x020 / 0x050 | 0x0c887f7f / 0x8c887f7f | codec A / B control; bit 31 held set while disabled |
| 0x024 / 0x054 | 0 / 0x8000 | sidetone (`ApmHW_SidetoneEnable`, `SidetoneVolumeSet`) |
| 0x02c / 0x05c | 0x00330000 / 0x00345a67 | DAC digital gain in the low 16 bits (A is muted) |
| 0x040 / 0x070 | 0x03130000 / 0x031329f1 | ADC digital gain in the low 16 bits (A is muted: exact zeros) |
| 0x044 / 0x074, 0x080, 0x084 | | sample-rate dividers and their load strobes (`ApmHw_SampleFreqSet`) |
| 0x120 | 0x40 | ADC analog front end power (`ApmHw_AmpSetPowerDown`): bits 2-3 codec A, bits 5-6 codec B, value 2 = on, 0 = off (so at idle B is on and A is off; Broadcom's driver uses this as the microphone mute). Bits 1 (codec A) and 4 (codec B) switch the input B microphone bias **off** when set (`ApmHw_SetMicBias`) |
| 0x124 / 0x12c | 0 / 0x02780000 | ADC analog front end of codec A / B: bit 25 selects input B (`ApmHW_SetTxMux`), bits 19-24 hold the preamp gain in dB (`ApmHw_PGAGainSet`, 0 to 42; 15 dB on codec B at idle) |
| 0x134 | 0x00053333 | output mute bits: 16 = B/out A, 17 = B/out B, 18 = A/out A, 19 = A/out B |
| 0x138 | 0xfdff3000 | output mode, two bits each (3 = off): bits 12-13 B/out A, 14-15 B/out B, 16-17 A/out A, 18-19 A/out B; output gains in the low bits |
| 0x200-0x284 | | codec C (ADC only) |
| 0x420-0x438 / 0x520-0x538 | 4, 4, 1, 1, 4 | FIFO thresholds and DMA request enables, codec A / B. Bit 1 of 0x42c / 0x52c switches the input A microphone bias off when set (it shares circuitry with microphone detection) |

FIFOs are separate 64 KB windows: 0x30670000 (A ADC), 0x30680000 (A DAC), 0x30690000 (B ADC), 0x306A0000 (B DAC).

GPIO31 is an external amplifier enable (**code**, `apmAnaGainSet` in `halaudio_apm.ko`, where a string calls it `extern_amp_gpio`): it is driven high when codec A's output A is unmuted and low when it is muted. `keypad.ko` calls the same pin `handfree_gpio`. **Measured:** it is an output at level 0 at idle under Yealink and under our kernel, so the external amplifier is off.

The clocks are `APM200` (200 MHz from PLL2) and `APM100`, both shown enabled in `/proc/clk/listClocks`, plus the "audio channel enable" register in chipc (mainline `chipcHw_audioChannelEnable`, bits for channels A, B, C and the APM and NTP clocks).

### What is likely but not proven

At idle Yealink keeps exactly one path alive: codec B, input B, preamp at 15 dB, ADC and DAC gains set, output B on and unmuted, sidetone on, mixer connected to the endpoint. Everything on codec A is muted and its ADC front end is powered off.

**Measured** with the capture driver, in a silent room: with the same 15 dB preamp, codec A's input A gives the same kind of noise floor as codec B's input B (about 15 counts rms), and it grows with the preamp gain, so something microphone-like is connected to both. Codec A's input B is ten times quieter (1.4 counts), as an input with nothing plugged in would be. That fits Reading 1 (codec A input B = the empty headset jack) but does not prove it.

- **Reading 1 (guess, about 60%):** this is the speakerphone, kept ready. It matches a Snom D375 on the same chip family with the same Broadcom modules, where input B of codec B was measured as the base microphone, codec A input A as the handset microphone and codec A input B as the headset jack (see "Other people's work"). The loudspeaker would then be codec B output B.
- **Reading 2 (guess, about 40%):** this is the handset path, the default device. In its favour: sidetone is switched on for codec B (register 0x54), which is a handset or headset feature and makes no sense for a speakerphone, and GPIO31, named "handfree" by Yealink, follows codec A output A, which would make codec A the speakerphone codec.

Daytime test 1 settles it and also gives Yealink's gain settings for every mode.

## Question 4: plan for our own driver

An ALSA driver (`kernel/drivers/t48s_audio.c`), not a private character device: PJSIP and baresip both speak ALSA, and so do `arecord`/`aplay` for testing. It needs `CONFIG_SOUND`/`CONFIG_SND` turned on (they are off in our config) and programs DMA controller 0 directly, the way `t48s_eth.c` does with controller 1 (our kernel deliberately leaves controller 0 alone).

| Step | What | Size | Test | Needs Tom awake? |
|---|---|---|---|---|
| 0 | User-space capture proof (`native/apmcap.c`) | done | done | no |
| 1 | **Done 2026-11-23.** ALSA capture: one card, two capture devices (codec A, codec B), 16 kHz, 16-bit mono, 5 ms periods, DMA0 channels 3 and 5 with their block interrupts (IRQ 3, 5), error interrupt (IRQ 28). Leaves the analog setup as Yealink left it. | 1 to 2 days | `arecord` from codec B, compare with `apmcap` | no |
| 2 | **Done 2026-11-23, tested on codec A only.** Capture controls: input select, preamp gain, ADC digital gain, ADC power, microphone bias, as ALSA mixer controls. Port of five short `ApmHw_*` functions. | 1 day | record each input | mostly no, see note |
| 3 | **Done 2026-11-23, see "Playback" below.** Playback: two playback devices on DMA0 channels 2 and 4, DAC digital gain, output mode and mute, amplifier power-down, GPIO31, ramp-down on stop (`ApmHw_AmpDacRampDown`) to avoid clicks. What the audio service expects of it, including control names, is in [audiod.md](audiod.md#what-the-kernel-driver-must-add). | 1 to 2 days | tone to each output at low level | **yes** |
| 4 | **Done 2026-11-24, see "Cold start" at the end; a start through U-Boot is still to be seen.** Cold start, so the live image (#7) works without Yealink having run first: clocks, chipc audio enable, APM soft reset, the `ApmHw_init` table (about 70 constant writes, already extracted), sample rate, FIFO setup. | 1 to 2 days | boot through U-Boot recovery, record and play | **yes** (possible click) |
| 5 | Paths as named ALSA controls ("Handset", "Headset", "Speaker"), sidetone for handset and headset, 8 kHz as a second rate. | 1 day | switch during a call | **yes** |
| 6 | In user space, with the SIP stack (#12): G.711/G.722 on the ARM, echo cancelling for the speakerphone (speexdsp is the first thing to try on this 500 MHz ARM11), ringer through the loudspeaker. | with #12 | calls | **yes** |

Note on step 2: capture-side writes are silent unless sidetone is on and an output is live, because sidetone feeds the microphone into the earpiece. The driver should switch sidetone off and mute the outputs before touching input settings in a night-time test; I did not try it tonight because I could not prove every output stage was off.

Why step 4 waited for daytime: a cold start begins with a soft reset of the whole APM block, and after a kexec boot codec B's output B is switched on and unmuted. What the analog output stage does across that reset is unknown (a click is likely), `ApmHw_init` then writes the DAC digital gains to full (0x2c and 0x5c = 0x00337fff), and it never writes the output mode and mute registers (0x134, 0x138), so their state after reset is whatever the chip's defaults are. None of that can be shown to be silent beforehand. The sequence is known: clocks, `chipcHw_audioChannelEnable`, `chipcHw_softReset(APM)`, the `ApmHw_init` writes (`tools/regsum.py ... c003f3e4 c003fa84` lists them), `ApmHw_SampleFreqSet`, `ApmHw_ChEnable`.

Risks: the equaliser coefficients (`ApmHw_WriteCoeff`) are loaded by Yealink at start-up and will be lost on a cold start; flat is fine to begin with. The speakerphone will need real echo-cancelling work. Codec C and the I2S/PCM ports are out of scope.

## The capture driver (2026-11-23, issue #24)

`kernel/drivers/t48s_audio.c`, built into our kernel by `kernel/build.sh` (copied to `sound/arm/`; `CONFIG_SND_T48S`). No DSP, no vendor module. If the voice DSP is still running after the kexec jump the driver halts it, with the same two writes as Yealink's `vpmHw_Halt`.

- Card 0, "T48S". `hw:0,0` is codec A, `hw:0,1` is codec B. Capture only: S16_LE, mono, 16000 Hz, nothing else offered. Periods are whole 5 ms blocks (multiples of 80 frames), buffers up to 2.56 s. mmap is supported.
- Controls, for each codec ("Codec A ...", "Codec B ..."): `Capture Switch` (ADC front end power, the microphone mute), `Capture Source` (0 = input A, 1 = input B), `Preamp Capture Volume` (0 to 14, 3 dB each, 0 to 42 dB), `Capture Volume` (digital: 0 = mute, n = n - 36 dB, so 36 is 0 dB and 48 is +12 dB), `Mic Bias A Switch`, `Mic Bias B Switch`.
- Counters and a register dump: `cat /proc/asound/card0/t48s`.
- The phone has no `arecord` or `amixer` yet, so two small static tools stand in, both in the kexec image: `t48rec` (`native/t48rec.c`, records a WAV and reports the measured rate, level and overruns) and `t48mix` (`native/t48mix.c`, lists or sets controls). They use the plain ALSA ioctls, so the real tools will work the same once the Buildroot image has them.

**It cannot make a sound by itself, with one caveat.** It never writes a DAC FIFO, DAC gain, output mode or mute, amplifier bit, sidetone or GPIO31. But codec B idles with its output B on and its sidetone on, so on codec B the microphone already reaches that output, and changing codec B's capture controls changes what goes there. On codec A nothing is connected through: its outputs are off, its DAC is muted and its sidetone is off. So tonight only codec A's controls were exercised; codec B was recorded as found and its controls only read. The playback step should take over sidetone and remove this trap.

(Since playback was added the driver uses channels 2 to 5; what follows describes the capture-only driver of that night.) How it takes DMA controller 0: our kernel's DMA framework stays patched to leave controller 0 completely alone (no channel reset at boot, no interrupt handlers), as before. The driver programs its two channels (3 and 5) directly, like `t48s_eth.c` does on controller 1, requests only their two interrupts, and uses the per-channel write-enable form of the shared registers, so channels 0, 1, 2, 4, 6 and 7 are never written. The kexec loader has already stopped all eight before our kernel starts.

**Measured** on the phone (kexec boot from Yealink 66.82.0.31, silent room):

| Check | Result |
|---|---|
| Rate, codec B, 5 s | 80000 frames in 5.016 s; 16000.0 frames/s between first and last read |
| Both codecs at once, 3 minutes | 2,880,000 frames each, 15999.5 and 16000.1 frames/s, no overruns |
| Interrupts over those 3 minutes | 9017 and 9465 period interrupts where 9001 each were expected (50 a second at 20 ms periods); APM error interrupt 0, DMA errors 0, ADC FIFO overflows 0, kernel "Err" count 0. No storm, but the DMA controller raises a few block interrupts nobody asked for: about 5 extra per 1000 with one codec recording, 50 per 1000 on the first-started codec when both record, none with 5 ms periods. They do no harm (ALSA checks the position and waits) |
| Period sizes | 80 frames (5 ms) x 2 up to 1600 frames x 2 all record cleanly |
| Noise floor, codec B as Yealink left it | rms 13.5 to 15.8 counts (-66 to -68 dBFS), mean -6; the same as `apmcap` and as Yealink's own `/proc/bcmapm/apm1` showed |
| Network while recording | 40 pings to the router at 4 a second, none lost; SSH stayed up throughout |
| Touchscreen and keypad | input devices still registered, touch chip still answers (`diag`: 36 C, 0 SPI errors); nobody was awake to press anything |
| GPIO31, output and sidetone registers | unchanged before and after (0x134 = 0x00053333, 0x138 = 0xfdff3000, 0x13c = 0x60000000, 0x54 = 0x8000, 0x5c = 0x00345a67, GPIO31 low) |
| CPU while both record | idle 100% in `top` |

Controls, on codec A:

- `Capture Switch`: off gives exact zeros even with gain set; on gives signal.
- `Capture Volume`: the DC level follows the dB steps to within 1% from -35 dB to +12 dB (137 counts at 0 dB, 275 at +6, 433 at +10, 545 at +12, 68.5 at -6, 13.8 at -20), including across the point where the register's shift field changes. 0 mutes to exact zeros.
- `Preamp Capture Volume`: noise on input A rises from 3.6 counts at 0 dB to 16 at 15 dB, 114 at 30 dB and 461 at 42 dB, about 3 dB a step. Above 18 dB a large DC offset appears (645 counts at 21 dB, 9000 at 42 dB): software will want a high-pass filter, or the gain kept at 18 dB or less.
- `Capture Source`: input A and input B give clearly different signals (16 against 1.4 counts at 15 dB).
- `Mic Bias A/B Switch`: the register bits change as intended, but in a silent room switching them made no measurable difference. Whether they really are the bias supplies needs a voice (daytime test 2).

Two things found on the way. The ADC never stops, so its FIFO sits full of old samples while nobody records: exactly 64 samples of whatever was last heard came out first. The driver now reads one 5 ms block into a scrap buffer at every start, which removes them. And `apmcap` and this driver cannot run at the same time (same DMA channels); `apmcap` is now only a historical proof.

Recordings from tonight (silent room, so only the noise floor): `dumps/audio/capture-2026-11-23-codecB.wav` (codec B as Yealink left it) and `-codecA.wav` (codec A, input A, 15 dB preamp, 0 dB digital), 10 s each, plus `-x100.wav` copies with the DC removed and the level raised 40 dB so there is something to hear.

Not done, and why: playback (out of scope and audible), cold start (above), sample rates other than 16 kHz, codec C.

## Daytime tests for Tom

1. **Which path is which (5 minutes, makes dial tone).** Under Yealink (`sh tools/ourkernel.sh back`), run `sh tools/audio-modes.sh idle`, then lift the handset and run it with `handset`, put it back and press Speaker (`speaker`), press Headset (`headset`). It only reads registers. The result lands in `dumps/audio/modes.txt` and fills in the table above, with Yealink's gains per mode.
2. **Hear the capture and find the microphones (5 minutes, silent on the phone).** Under our kernel, talk near the base, then into the handset, during each of these:
   - `t48rec -d 1 -s 10 -o /tmp/b.wav` (codec B exactly as Yealink left it);
   - `t48mix "Codec A Capture Switch" 1; t48mix "Codec A Preamp Capture Volume" 5; t48mix "Codec A Capture Volume" 36`, then `t48rec -d 0 -s 10 -o /tmp/a-inA.wav`;
   - `t48mix "Codec A Capture Source" 1`, then `t48rec -d 0 -s 10 -o /tmp/a-inB.wav` (plug a headset in for this one if there is one).

   Copy the files off (`sh tools/ourkernel.sh ssh 'cat /tmp/b.wav' > b.wav`) and listen. This says which input is which microphone. Then, on whichever codec A input has a voice on it, switch its `Mic Bias` control off and record again: the voice should go, which confirms the bias controls.
2b. **Codec B's controls (2 minutes, may be faintly audible).** `t48mix "Codec B Preamp Capture Volume" 3`, record, set it back to 5; the same with `Codec B Capture Source` 0 then 1. These change what the sidetone sends to codec B's live output, which is why they were not tried at night.
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

- `kernel/drivers/t48s_audio.c`: the ALSA driver, capture and playback.
- `native/t48rec.c`, `native/t48play.c`, `native/t48mix.c`: record, play and set controls on the phone without alsa-utils.
- `native/apmcap.c`: the first capture proof, from user space. Records only. Not for use alongside the driver.
- `tools/regsum.py`: lists the register accesses of each function in Yealink's kernel image.
- `tools/audio-modes.sh`: daytime test 1.
- `dumps/audio/` (not committed): the six modules, the DSP image, `kallsyms.txt`, the idle register dump, the `ApmHw_*` and `vpmHw_*` register summaries (`apmhw.sum`, `vpmhw.sum`), the `/proc` captures and the oops log.

## Daytime test 1 result (2026-11-23, under Yealink 66.82.0.31)

Register differences between Yealink's modes (`tools/audio-modes.sh`; raw in `dumps/audio/modes.txt`, not committed):

| Register | idle | handset | speaker | headset (none plugged) |
|---|---|---|---|---|
| 0x3060002c | 00330000 | 00330000 | 00334027 | 00330000 |
| 0x30600040 | 03130000 | 03130000 | 03132f0f | 03130000 |
| 0x30600054 | 00008000 | 00008000 | 00000000 | 00008000 |
| 0x3060005c | 00345a67 | 00345a67 | 00330000 | 00345a67 |
| 0x30600070 | 031329f1 | 031329f1 | 03130000 | 031329f1 |
| 0x30600120 | 00000040 | 00000040 | 00000008 | 00000040 |
| 0x30600124 | 00000000 | 00000000 | 00a80000 | 00000000 |
| 0x3060012c | 02780000 | 02780000 | 02000000 | 00b00000 |
| 0x30600134 | 00053333 | 00053333 | 00033333 | 00063333 |
| 0x30600138 | fdff3000 | fdff3000 | e7fcf000 | ffafc000 |
| 0x80004000 (GPIO 0-31) | 6f7bffff | 4f7bffff | ef7bffdf | 6f7bfff7 |

What it settles:

- **Codec B is the handset**, and Yealink keeps it set up at idle; lifting the handset changes no audio register, only GPIO29 (the hook) goes low.
- **Codec A plus the external amplifier (GPIO31 high) is the speakerphone.** Codec B is switched off in speaker mode (0x3060005c and 0x30600070 drop to their idle-off values, codec B sidetone 0x30600054 off).
- **Headset uses codec B with a different analog input/output selection** (0x3060012c, 0x30600134, 0x30600138 change; codec B digital registers unchanged).
- The key lamps show up as expected: GPIO5 low in speaker mode (Speakerphone lamp), GPIO3 low in headset mode (Headset lamp).

So "reading 2" above was right, not reading 1: `audiod`'s compiled-in route table must swap codecs (handset and headset on codec B, speaker on codec A + amp), and the analog values above are Yealink's settings to copy per route.

## Daytime test 2 result (2026-11-23, our kernel, `t48rec`)

Tom counted aloud into the handset, then at the phone from about 50 cm (8 s each, both codecs recording at once; files in `dumps/audio/test2/`, not committed):

| Recording | Loudest 1 s | Quietest 1 s | Verdict |
|---|---|---|---|
| handset, codec B input B (as Yealink left it: preamp 5, digital 36) | -30.9 dBFS | -68.7 dBFS | handset microphone |
| handset, codec A input A | -60.0 | -69.1 | faint pickup only |
| at the phone, codec A input A (preamp 5, digital 36) | -51.5 | -68.1 | base (speakerphone) microphone; needs more gain |
| at the phone, codec A input B | -84.4 | -90.3 | nothing connected |
| at the phone, codec B input B | -46.8 | -52.8 | handset mic hearing the room from the cradle |

So: handset microphone = codec B input B; speakerphone microphone = codec A input A (about 20 dB more gain wanted for speech at 50 cm); codec A input B is unused (probably the headset mic is codec B's other input, selected by 0x3060012c/134/138 in headset mode, untested without a headset).

## Playback (2026-11-23, issue #24)

The driver now plays as well as records. Still no DSP and no vendor module.

- `hw:0,0` (codec A) and `hw:0,1` (codec B) each have a playback side: S16_LE, mono, 16000 Hz, periods in whole 5 ms blocks, DMA0 channels 2 and 4. Capture is unchanged and runs alongside on the same clock.
- New controls, for each codec: `Output A Playback Switch`, `Output B Playback Switch` (the analog output stage: on, or muted and off), `Output A Playback Volume`, `Output B Playback Volume` (analog, 0 to 7 in 3 dB steps, 7 is 0 dB), `Playback Volume` (DAC digital: 0 = mute, n = n - 36 dB, up to 42 = +6 dB), `Sidetone Switch`, `Sidetone Volume` (0 = mute, n = n - 36 dB, 36 at most). And one for the board: `Speaker Amp Switch` (GPIO31).
- Which is which (from the daytime tests): codec A output A is the loudspeaker, through the amplifier; codec B output B is the handset earpiece; codec B output A is the headset. Codec A output B is unused.
- `t48play` (`native/t48play.c`) plays a test tone or a WAV file, like `t48rec` records. It refuses tones above -6 dBFS.

**Quiet by default.** At start-up the driver switches the amplifier off, both sidetones off, mutes and switches off all four outputs, sets every analog output gain to its lowest (-21 dB) and both DAC gains to -20 dB. That also removes the trap described above (codec B left live with sidetone by Yealink). Nothing can be heard until user space switches an output on, and the amplifier is only ever switched on by its own control; switching the loudspeaker output off switches the amplifier off too.

**No clicks.** An output is switched on while still muted and then unmuted, and muted before it is switched off; the mute is the codec's own 40 ms ramp, which Yealink had already set up (registers 0x134 low half and 0x13c) and the driver leaves alone. A stream starts from a block of zeros. When a stream stops, is dropped or underruns, the DMA is steered to a 5 ms fade from the last sample played down to zero and then round a block of zeros, so the DAC is never cut off in mid-wave or left starved while an output is on. The DMA channel stops only when the stream is closed.

Register details found on the way (all from `ApmHw_*` in Yealink's kernel, checked against the three modes of daytime test 1):

| Register | Bits | What |
|---|---|---|
| 0x134 | 16 to 19 | output mutes: 16 = codec B output A, 17 = B/B, 18 = A/A, 19 = A/B |
| 0x134 | 0 to 15 | one nibble per output: ramp time ((n + 1) x 20 ms in bits 1 to 3) and a ramp enable; 0x3333 as found |
| 0x138 | 12 to 19 | output mode, two bits each: 0 = on, 3 = off (order B/A, B/B, A/A, A/B) |
| 0x138 | 20 to 31 | analog output attenuation, three bits each, 3 dB a step, 7 = -21 dB (order B/A, B/B, A/A, A/B). Yealink: handset 3 (-9 dB), headset 2 (-6 dB), loudspeaker 1 (-3 dB) |
| 0x13c | 9, 10 | DAC power down, codec B, codec A (not used here) |
| 0x02c / 0x05c | 0 to 18 | DAC digital gain: 15-bit gain where 0x7fff is 0 dB, and a shift in bits 16 to 18 (3 = none). Yealink: loudspeaker -6 dB (0x4027), handset and headset +3 dB (0x5a67 with shift 4) |
| 0x024 / 0x054 | 15 | sidetone on. Its level is one of 28 filter coefficients written through 0x010 (value) and 0x014 (index, bank, strobe) |

One thing that was not as expected: the DAC channels' DMA control word (0x00904835, as Yealink has it) counts a block in 64-bit units of memory, so its block count is 20 where the ADC's is 40. With 40 the sound played at half speed, each block running on into the next.

**Measured** on the phone, using its own microphones as the listener (nobody had to listen; levels are rms):

| Check | Result |
|---|---|
| Rate | 32000 frames played in 2.016 s on each codec, first write to end of drain (16 kHz); no DMA errors, no FIFO underflows |
| Loudspeaker, 1 kHz at -30 dBFS, Yealink's speaker gains (DAC -6 dB, output -3 dB, amplifier on) | -24.5 dBFS at the base microphone (preamp 21 dB, digital +12 dB), steady within 0.5 dB over 2 s, 25 dB above everything else in the recording; room and microphone noise is at -49 dBFS |
| Output gain control | 9 dB down on the control gave 9.1 dB down at the microphone |
| Edges | recorded through the microphone: tone start, tone end, output switched on and off, amplifier on and off, the player killed in mid-tone, and a forced underrun. None left a step bigger than the tone's own slope, and with the tone filtered out nothing stood out of the noise at any of them |
| Handset earpiece, same tone, Yealink's handset gains | with the handset on its cradle the handset microphone hears it only just: about 10 dB over its own noise in a narrow band with the microphone gain turned right up (-84 dBFS at its normal gain), and the base microphone hears it too. With the output muted, nothing. So the earpiece plays and the switch works; how it sounds needs an ear |
| Loudspeaker to base microphone, by frequency (same gains) | the microphone reads this much against the level in the frame played: 300 Hz 0 dB, 500 -5, 700 -1, 1000 +5, 1400 +7, 2000 +1, 2800 -15, 3600 -20 dB. The loop takes 80 ms |

Not measured, needs an ear: sidetone (the level control is a straight port of Yealink's function and could not be checked by loopback), how the earpiece and loudspeaker sound, and the headset path with a headset plugged in.

**The loudspeaker can freeze the phone** (2026-11-24, issue #57): with these gains a steady tone above about -18 dBFS, less when the processor is busy, pulls the supply down until the phone hangs. `t48-audiod` limits what it sends to -25.2 dBFS ([audiod.md](audiod.md#the-loudspeakers-limit)); `t48play` does not, so keep test tones well under that.

To hear it (moderate levels; nothing here is louder than quiet speech):

```
t48mix "Codec A Playback Volume" 30; t48mix "Codec A Output A Playback Volume" 6
t48mix "Codec A Output A Playback Switch" 1; t48mix "Speaker Amp Switch" 1
t48play -d 0 -s 2 -l -30            # 1 kHz from the loudspeaker for 2 s
t48mix "Codec A Output A Playback Switch" 0     # also switches the amplifier off

t48mix "Codec B Playback Volume" 39; t48mix "Codec B Output B Playback Volume" 4
t48mix "Codec B Output B Playback Switch" 1
t48play -d 1 -s 2 -l -30            # the same in the handset earpiece
t48mix "Codec B Sidetone Switch" 1  # now blow across the handset microphone: you should hear it
t48mix "Codec B Sidetone Switch" 0; t48mix "Codec B Output B Playback Switch" 0
```

## Cold start (2026-11-24, issues #7 and #29)

**What went wrong.** On the first start of Linux 6.18 through U-Boot's recovery mode the kernel stopped 2.6 s in, inside this driver. U-Boot does nothing with audio, so the block's clocks are off, and a register of a block without its clock does not answer: the bus raises an external abort and the kernel has nobody to give it to. The driver had read the block before asking whether it was on.

**What the driver does now** (`kernel6/drivers/t48s_audio.c`, and the same code in `kernel/drivers/t48s_audio.c`):

1. It reads chip control (0x80000000), never the block, to see whether the block is on: the two APM clocks (0x44, 0x58), the audio channel enables (0x7c), the soft reset (0x84) and DMA controller 0's bus clock (0x5c). The values found are one line in the boot log.
2. If anything is off it starts the block: the amplifier pin low, clocks on, audio channels enabled, a soft reset, the `ApmHw_init` table, 16 kHz on both codecs (`ApmHw_SampleFreqSet`), the codec enables (`ApmHw_ChEnable`), then the analog side as Yealink idles. All four outputs are muted, switched off and at their lowest gain before anything runs, and the DACs are powered last.
3. The first accesses are made with the external abort caught. A block that still does not answer costs the sound, not the boot: the log says `no sound: the audio block could not be started` and no card is registered.

**The test hook.** `echo 1 > /sys/module/t48s_audio/parameters/cold` (audio service stopped, no stream open) puts the block back where a power-on leaves it (in reset, clocks gated, channels disabled, DMA controller off), reads it once to show that the read is refused and caught, and starts it again:

```
t48s_audio: test: audio block switched off (clocks gated)
t48s_audio: test: a read of the block while off was refused, and caught
t48s_audio: audio block is off (switched off for the test): starting it
t48s_audio: audio block started from cold
```

**Measured** on the phone after that forced cold start, from a kexec start (so not yet after U-Boot alone):

| Check | Before (as Yealink left it) | After the cold start |
|---|---|---|
| Rate, both codecs | 15999.7 to 15999.9 frames/s | 15999.8 to 16002.0 frames/s |
| Handset microphone, quiet room (codec B input B, daytime test 2 settings) | -67.2 dBFS | -66.4, -66.1 dBFS |
| Base microphone (codec A input A) | -67.1 dBFS | -67.7, -67.8 dBFS |
| Codec A input B (nothing connected) | -87.9 dBFS | -91.5, -91.8 dBFS |
| Loudspeaker to base microphone, tones at -30 dBFS, the "Playback" settings: 300 / 500 / 1000 / 2000 / 3000 Hz | -28.0 / -33.1 / -33.0 / -17.4 / -36.4 dBFS | -31.1 / -33.3 / -36.3 / -16.9 / -27.3, then -31.3 / -32.4 / -37.5 / -17.8 / -26.5 dBFS |
| A call to the echo service, speaker route, G.722, 13 and 15 s | | 785 and 679 frames each way; the far end's level follows the microphone's second by second, 210 ms later, within 0.1 dB overall |
| Under Linux 3.6 | | read refused and caught, block started, 16000.1 frames/s, 2 kHz loop tone at -17.5 dBFS |

Nobody was talking during the call, so the comparison is of room noise: the levels track, but the correlation is low (0.2 to 0.5). It wants a voice.

**Clicks.** With both microphones recording after the cold start: powering the DACs down and up (the last step of the start), switching the earpiece on and off, and switching the loudspeaker and its amplifier on and off left nothing above the microphones' noise. The reset itself cannot be heard this way, because the microphones belong to the block being reset; that needs an ear at the first real cold start. The amplifier pin is low throughout.

**What is different after a cold start.** The equalisers are off (flat). Yealink loads its own tuning into them, and a phone started by kexec still has it; a phone started from cold does not. That is the 3 dB at 1 kHz and the 9 dB at 3 kHz in the loop row above. The loudspeaker's limit ([audiod.md](audiod.md#the-loudspeakers-limit)) was measured with Yealink's tuning in place, so it should be checked again on a phone started from cold.

**Still to be seen:** a start through U-Boot with this driver. What U-Boot leaves in chip control is not known beyond "the block does not answer"; the boot log line `chip control as found` will say. If U-Boot's PLL2 setting differs from the one the 200 MHz clock divider assumes, the driver warns and the sample rate will be off: `t48rec` prints the rate.
