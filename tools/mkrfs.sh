#!/bin/sh
# mkrfs.sh INIT OUT.rfs: build the ext2 ramdisk for U-Boot's USB/TFTP recovery boot.
# INIT becomes /sbin/init. Also includes the phone's userenv tool with its C library
# (copied from the phone over SSH) and the device nodes the test init needs.
# Needs sudo (for mke2fs -d ownership and debugfs) and the phone reachable as $PHONE.
set -e
INIT=$1; OUT=$2; PHONE=${PHONE:-10.45.0.214}
S=$(mktemp -d)
mkdir -p $S/sbin $S/lib $S/usr/sbin $S/proc $S/dev $S/tmp
cp "$INIT" $S/sbin/init
scp -q -O root@$PHONE:/lib/ld.so.1 root@$PHONE:/lib/libc.so.6 root@$PHONE:/usr/sbin/userenv $S/lib/
mv $S/lib/userenv $S/usr/sbin/userenv
ln -s ld.so.1 $S/lib/ld-linux.so.3
chmod 755 $S/sbin/init $S/usr/sbin/userenv
sudo chown -R root:root $S
rm -f "$OUT"
# 1 KB blocks and 128-byte inodes, no newer features: the phone's kernel is 2.6.27
sudo mke2fs -q -t ext2 -I 128 -b 1024 -m 0 -O ^dir_index,^resize_inode -L t48s -d $S "$OUT" 4096 2>/dev/null
# device nodes cannot be created in an unprivileged container, so add them inside the image
printf 'cd /dev\nmknod console c 5 1\nmknod ttyAMA0 c 204 64\nmknod null c 1 3\nmknod mem c 1 1\nmknod mtd2 c 90 4\nmknod mtd3 c 90 6\n' > $S.cmd
for f in console ttyAMA0 null mem mtd2 mtd3; do printf 'sif /dev/%s mode 020600\n' $f >> $S.cmd; done
sudo debugfs -w -f $S.cmd "$OUT" >/dev/null 2>&1
sudo chown "$(id -u):$(id -g)" "$OUT"
sudo rm -rf $S $S.cmd
sudo e2fsck -fn "$OUT" | tail -n 1
