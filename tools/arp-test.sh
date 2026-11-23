#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# arp-test.sh [IDLE_SECONDS]: does the phone still answer ARP from another host (issue 34)?
# Leaves the phone alone for IDLE_SECONDS (default 0), then has proxmox0 forget the
# phone's MAC address and ping it, so the ping has to start with a broadcast ARP request
# that the phone must receive and answer. Prints PASS or FAIL.
PHONE=${PHONE:-10.45.0.214}; HOST=${ARP_HOST:-root@proxmox0}
sleep "${1:-0}"
out=$(ssh -o BatchMode=yes "$HOST" "ip neigh del $PHONE dev \$(ip -o route get $PHONE | sed 's/.* dev \([^ ]*\).*/\1/') 2>/dev/null; ping -c 3 -W 1 $PHONE; ip neigh show $PHONE")
if echo "$out" | grep -q " 0% packet loss"; then
    echo "PASS: ARP answered after ${1:-0} s idle ($(echo "$out" | tail -n 1))"
else
    echo "FAIL: no answer to ARP after ${1:-0} s idle ($(echo "$out" | tail -n 1))"; exit 1
fi
