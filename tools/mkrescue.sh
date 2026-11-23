#!/bin/sh
# mkrescue.sh OUTDIR: build the rescue set (T48S.bin, T48S.rfs, T48S.rom) for U-Boot's
# recovery mode, option 1 (serve with tools/tftpd.py OUTDIR 69 --rom-once).
# T48S.bin is Yealink's own kernel (NAND drivers built in); T48S.rfs runs
# tools/rescue-init.sh, which clears the boot-mode flags, puts our boot hook and SSH
# back, prints diagnostics on the serial port and resets via the watchdog.
# Needs: dumps/usbtest (Yealink kernel, ROM, a ramdisk holding Yealink's libc),
# dumps/userenv-usr-sbin, the musl busybox and dropbear builds, sudo.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd); OUT=$1; T=$HOME/src-build/t48s
mkdir -p "$OUT"; S=$(mktemp -d)
mkdir -p $S/sbin $S/bin $S/lib $S/usr/sbin $S/proc $S/dev $S/tmp $S/cfg $S/dat $S/rfs $S/payload/ssh $S/payload/custom
for f in ld.so.1 libc.so.6; do sudo debugfs -R "dump /lib/$f $S/lib/$f" "$HERE/dumps/usbtest/T48S.rfs" >/dev/null 2>&1; done
ln -s ld.so.1 $S/lib/ld-linux.so.3
cp "$HERE/dumps/userenv-usr-sbin" $S/usr/sbin/userenv
cp $T/busybox-1.36.1/busybox $S/bin/busybox && ln -s busybox $S/bin/sh
cp $T/dropbear-2025.88/dropbearmulti $S/payload/ssh/
cat ~/.ssh/id_ed25519.pub "${EXTRA_KEYS:-$HOME/.config/yealink-t48s/authorized_keys.extra}" > $S/payload/ssh/authorized_keys  # extra owner keys live outside the repo
cp "$HERE"/phone/simulator.sh "$HERE"/phone/start.sh "$HERE"/phone/rearm.sh $S/payload/custom/
(cd "$HERE/native" && make -s fbswap.so envexec tap >/dev/null && cp fbswap.so envexec tap $S/payload/custom/)
cp "$HERE/tools/rescue-init.sh" $S/sbin/init
sudo chmod 755 $S/sbin/init $S/bin/busybox $S/usr/sbin/userenv $S/lib/ld.so.1 $S/lib/libc.so.6
sudo chown -R root:root $S
rm -f "$OUT/T48S.rfs"
sudo mke2fs -q -t ext2 -I 128 -b 1024 -m 0 -O ^dir_index,^resize_inode -L t48s-rescue -d $S "$OUT/T48S.rfs" 6144 2>/dev/null
{ printf 'cd /dev\n'; for n in "console c 5 1" "ttyAMA0 c 204 64" "null c 1 3" "mem c 1 1" "mtd2 c 90 4" "mtd3 c 90 6" "mtdblock8 b 31 8" "mtdblock11 b 31 11" "mtdblock12 b 31 12"; do printf 'mknod %s\n' "$n"; done
  for f in console ttyAMA0 null mem mtd2 mtd3; do printf 'sif /dev/%s mode 020600\n' $f; done
  printf 'sif /dev/null mode 020666\n'; for b in 8 11 12; do printf 'sif /dev/mtdblock%s mode 060600\n' $b; done; } > $S.cmd
sudo debugfs -w -f $S.cmd "$OUT/T48S.rfs" >/dev/null 2>&1
sudo chown "$(id -u):$(id -g)" "$OUT/T48S.rfs"; sudo rm -rf $S $S.cmd
cp "$HERE/dumps/usbtest/T48S.bin" "$HERE/dumps/usbtest/T48S.rom" "$OUT/"
sudo e2fsck -fn "$OUT/T48S.rfs" | tail -n 1
