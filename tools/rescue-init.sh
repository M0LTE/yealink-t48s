#!/bin/sh
# /sbin/init of the rescue ramdisk (tools/mkrescue.sh). Booted with Yealink's own
# kernel through U-Boot's recovery mode, so Yealink's NAND and yaffs2 drivers are
# available. Puts our boot hook and SSH back on the phone, clears BOOTMODE,
# then resets the board through the watchdog. Output goes to the serial port.
export PATH=/bin:/usr/sbin
/bin/busybox --install -s /bin
mount -t proc proc /proc
stty -F /dev/ttyAMA0 115200 raw -echo 2>/dev/null
exec >/dev/ttyAMA0 2>&1
echo
echo "===== T48S rescue ramdisk ====="
echo "--- userenv before:"; userenv
# a failed firmware update leaves fwup_mode=safeup and fwup_install=1, and U-Boot
# then forces BOOTMODE=safemode on every boot; clear all three
userenv -s BOOTMODE
userenv -s fwup_mode
userenv -s fwup_install -v 0
echo "--- userenv after:"; userenv
mkdir -p /rfs && mount -t yaffs2 -o ro /dev/mtdblock8 /rfs && { echo "--- /usr/scripts/safemode.sh:"; cat /rfs/usr/scripts/safemode.sh; umount /rfs; }
mount -t yaffs2 /dev/mtdblock11 /cfg && echo "config partition mounted"
mount -t yaffs2 /dev/mtdblock12 /dat && echo "data partition mounted"
echo "--- before:"; ls -la /cfg; ls -la /dat /dat/ssh /dat/custom
mkdir -p /dat/ssh /dat/custom
[ -x /dat/ssh/dropbearmulti ] || cp /payload/ssh/dropbearmulti /dat/ssh/
(cd /dat/ssh && for p in dropbear dropbearkey scp dbclient; do ln -sf dropbearmulti $p; done)
cp /payload/ssh/authorized_keys /dat/ssh/authorized_keys
chmod 700 /dat/ssh; chmod 600 /dat/ssh/authorized_keys; chmod 755 /dat/ssh/dropbearmulti
cp /payload/custom/* /dat/custom/ && chmod 755 /dat/custom/*
rm -f /cfg/simulator.sh.armed
cp /payload/custom/simulator.sh /cfg/simulator.sh && chmod 755 /cfg/simulator.sh
sync
echo "--- after:"; ls -la /cfg/simulator.sh /dat/ssh /dat/custom
umount /cfg; umount /dat; sync
echo "rescue done; resetting through the watchdog in about 4 s"
devmem 0x80017c00 32 0x1ACCE551; devmem 0x80017800 32 50000000; devmem 0x80017808 32 3
while true; do sleep 60; done
