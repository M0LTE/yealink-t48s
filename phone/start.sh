#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Our services, started early in boot by /config/simulator.sh.
D=/data/ssh
[ -f $D/dropbear_ed25519_host_key ] || $D/dropbearkey -t ed25519 -f $D/dropbear_ed25519_host_key
$D/dropbear -r $D/dropbear_ed25519_host_key -D $D -s -p 22 -P /var/dropbear.pid
echo "dropbear started"

# Our screen (GB7RDG status), kept running
if [ -x /data/custom/t48ui ]; then
    ( while true; do
        /data/custom/t48ui >> /tmp/t48ui.log 2>&1
        sleep 5
    done ) &
    echo "t48ui started"
fi
