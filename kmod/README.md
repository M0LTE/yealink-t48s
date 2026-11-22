# Kernel modules for Yealink's kernel

**Working (2026-11-22): our kernel boots from Yealink's running system with no key presses.** One cycle:

```
tools/kexec-boot.sh dumps/Image-kexec 90                     # jump to our kernel (about 1 s)
ssh root@proxmox0 'python3 /root/t48s/sercmd.py "uname -a"'  # use its serial shell
ssh root@proxmox0 'python3 /root/t48s/sercmd.py back-to-yealink'   # SSH to Yealink again ~55 s later
```

What it took, in order: build modules in a 2.6.27.47 tree matching Yealink's ABI; convert addresses with Yealink's RAM base (0x00400000), not the build tree's; set UARTA to 115200 before the jump (Yealink leaves it at 9600); skip DMA setup in our kernel (the DMA block faults after Yealink's kernel has run); freeze `dskPhone.exx` before writing 0 to `/proc/keypad/watchdog` (the UI re-arms the keypad chip's 11 s watchdog); re-arm our self-disarming boot hook before jumping; stop every channel on both DMA controllers in the stub (Yealink's audio DMA rings sit at 0x0752xxxx-0x076exxxx and keep writing into what becomes our kernel's RAM, corrupting it at random). The SP805 watchdog (armed by the loader, disarmed by our init) resets to Yealink if our kernel hangs early.

Built against vanilla Linux 2.6.27.47 prepared to match Yealink's module ABI (vermagic `2.6.27.47 preempt mod_unload ARMv6`, no symbol versions, `struct module` 0x140 bytes): `prepare-2.6.27.sh` (OMAP 2430 ARMv6 defconfig without MODVERSIONS). Then, in the t48s-kbuild container:

```
make -C linux-2.6.27.47 ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- M=/w/kmod modules
```

- `hello.ko`: smoke test.
- `t48kexec.ko`: boots our own kernel from Yealink's running kernel, no key presses and no flash writes. `insmod t48kexec.ko image=/tmp/Image wdt=60`. Reads the raw Image into pages outside the destination, jumps to a stub (`t48kexec_stub.S`) that turns the MMU and caches off, copies the image to 0x00408000, arms the SP805 watchdog (reset after `wdt` seconds; our kernel's init disarms it) and enters the kernel with r1 = 2342. Before loading it, disable the keypad chip's watchdog (`echo 0 > /proc/keypad/watchdog`) and stop network DMA (`ifconfig eth0 down`).

The kernel to load is `kernel/build.sh kexec` (`Image-kexec`, root filesystem built in from `kernel/kexec-rootfs.list`). From its shell, `back-to-yealink` resets the phone through the watchdog.
