#!/bin/sh
# Put the boot hook back once this boot has been up for 2 minutes with SSH running.
sleep 120
if [ -f /var/dropbear.pid ] && kill -0 $(cat /var/dropbear.pid) 2>/dev/null; then
    [ -f /config/simulator.sh.armed ] && mv /config/simulator.sh.armed /config/simulator.sh
    sync
    echo "re-armed"
else
    echo "dropbear not running; leaving the hook disarmed"
fi
