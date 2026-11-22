#!/bin/sh
# Our services, started early in boot by /config/simulator.sh.
D=/data/ssh
[ -f $D/dropbear_ed25519_host_key ] || $D/dropbearkey -t ed25519 -f $D/dropbear_ed25519_host_key
$D/dropbear -r $D/dropbear_ed25519_host_key -D $D -s -p 22 -P /var/dropbear.pid
echo "dropbear started"
