#!/bin/sh
# Put the boot hook back once SSH is running. The hook disarms itself at the start of
# every boot so a broken setup falls back to stock Yealink; SSH being up is the proof
# that this boot is fine, so re-arm 15 s after it is seen (a long window here once let
# a quick reboot land on a boot without SSH).
i=0
while [ $i -lt 60 ]; do
    i=`expr $i + 1`
    if [ -f /var/dropbear.pid ] && kill -0 $(cat /var/dropbear.pid) 2>/dev/null; then
        sleep 15
        [ -f /config/simulator.sh.armed ] && mv /config/simulator.sh.armed /config/simulator.sh
        sync
        echo "re-armed"
        exit 0
    fi
    sleep 2
done
echo "dropbear not running after 2 minutes; leaving the hook disarmed"
