#!/bin/sh
# mkrfs-bb.sh BUSYBOX OUT.rfs: ext2 ramdisk for our own kernel: static busybox as
# init, a shell on the serial console, and a banner on the screen (native/membanner).
# Needs sudo (mke2fs -d ownership, debugfs).
set -e
BB=$1; OUT=$2; HERE=$(cd "$(dirname "$0")/.." && pwd)
S=$(mktemp -d)
mkdir -p $S/bin $S/sbin $S/usr/bin $S/usr/sbin $S/etc/init.d $S/proc $S/sys $S/dev $S/tmp $S/root
cp "$BB" $S/bin/busybox
ln -s busybox $S/bin/sh
ln -s ../bin/busybox $S/sbin/init
cp "$HERE/native/membanner" $S/usr/bin/membanner
cat > $S/etc/inittab <<'EOT'
::sysinit:/etc/init.d/rcS
ttyAMA0::respawn:-/bin/sh
::ctrlaltdel:/sbin/reboot
EOT
cat > $S/etc/init.d/rcS <<'EOT'
#!/bin/sh
/bin/busybox --install -s
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null
echo
echo "===== T48S: our own kernel is running ====="
uname -a
cat /proc/cmdline
free
/usr/bin/membanner "OUR KERNEL" "$(uname -sr) $(uname -v)" "$(grep -m1 Hardware /proc/cpuinfo)" "$(grep -m1 MemTotal /proc/meminfo)" "busybox shell on the serial console"
EOT
chmod 755 $S/bin/busybox $S/usr/bin/membanner $S/etc/init.d/rcS
echo 'export PS1="t48s# "' > $S/etc/profile
sudo chown -R root:root $S
rm -f "$OUT"
sudo mke2fs -q -t ext2 -I 128 -b 1024 -m 0 -O ^dir_index,^resize_inode -L t48s -d $S "$OUT" 4096 2>/dev/null
printf 'cd /dev\nmknod console c 5 1\nmknod ttyAMA0 c 204 64\nmknod null c 1 3\nmknod mem c 1 1\nsif /dev/console mode 020600\nsif /dev/ttyAMA0 mode 020600\nsif /dev/null mode 020666\nsif /dev/mem mode 020600\n' > $S.cmd
sudo debugfs -w -f $S.cmd "$OUT" >/dev/null 2>&1
sudo chown "$(id -u):$(id -g)" "$OUT"
sudo rm -rf $S $S.cmd
sudo e2fsck -fn "$OUT" | tail -n 1
