# Kernel modules for Yealink's kernel

Built against vanilla Linux 2.6.27.47 prepared to match Yealink's module ABI (vermagic `2.6.27.47 preempt mod_unload ARMv6`, no symbol versions, `struct module` 0x140 bytes): `prepare-2.6.27.sh` (OMAP 2430 ARMv6 defconfig without MODVERSIONS). Then, in the t48s-kbuild container:

```
make -C linux-2.6.27.47 ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- M=/w/kmod modules
```

- `hello.ko`: smoke test.
- `t48kexec.ko`: boots our own kernel from Yealink's running kernel, no key presses and no flash writes. `insmod t48kexec.ko image=/tmp/Image wdt=60`. Reads the raw Image into pages outside the destination, jumps to a stub (`t48kexec_stub.S`) that turns the MMU and caches off, copies the image to 0x00408000, arms the SP805 watchdog (reset after `wdt` seconds; our kernel's init disarms it) and enters the kernel with r1 = 2342. Before loading it, disable the keypad chip's watchdog (`echo 0 > /proc/keypad/watchdog`) and stop network DMA (`ifconfig eth0 down`).

The kernel to load is `kernel/build.sh kexec` (`Image-kexec`, root filesystem built in from `kernel/kexec-rootfs.list`). From its shell, `back-to-yealink` resets the phone through the watchdog.
