#!/bin/sh
# Boot hook for the custom T48S setup, installed as /config/simulator.sh.
# Yealink's /phone/scripts/phone.sh runs this file instead of its normal startup
# whenever it exists.
#
# Safety net: move this hook out of the way first, so a hang or reboot loop falls
# back to stock Yealink on the next boot. rearm.sh puts it back once this boot has
# gone well.
mv /config/simulator.sh /config/simulator.sh.armed
sync

[ -x /data/custom/start.sh ] && /data/custom/start.sh > /tmp/custom-start.log 2>&1 &
[ -x /data/custom/rearm.sh ] && /data/custom/rearm.sh > /tmp/custom-rearm.log 2>&1 &

# The hook file is gone now, so Yealink's own script takes its normal path.
exec /phone/scripts/phone.sh
