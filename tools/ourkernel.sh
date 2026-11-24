#!/bin/sh
# ourkernel.sh boot [IMAGE]   : start our kernel from Yealink and wait for its shell prompt.
#                              IMAGE defaults to dumps/Image-kexec: Linux 6.18 (kernel6/) since issue 55.
#                              The 3.6 fallback, Image-kexec-linux3.6, starts the same way.
# ourkernel.sh cmd "COMMAND" [SECONDS] : run a command in its serial shell, print the output
# ourkernel.sh ssh ["COMMAND"] : SSH into our kernel (DHCP + dropbear, key login; fresh host key each boot).
#                              Needs a development build, the only kind with SSH on from boot (docs/access.md)
# ourkernel.sh back           : reset to Yealink and wait until SSH answers again
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd); PHONE=${PHONE:-10.45.0.214}
case "$1" in
boot)
    ssh -o BatchMode=yes root@proxmox0 "python3 /root/t48s/waitfor.py 't48s# ' 60" &
    W=$!
    sleep 1
    "$HERE/tools/kexec-boot.sh" "${2:-$HERE/dumps/Image-kexec}" 90
    wait $W ;;
cmd)
    ssh -o BatchMode=yes root@proxmox0 "python3 /root/t48s/sercmd.py \"$2\" ${3:-3}" ;;
ssh)
    shift
    exec ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR root@$PHONE "$@" ;;
back)
    # over SSH if our kernel's network is up, else the serial shell
    timeout 10 ssh -o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
        root@$PHONE 'back-to-yealink >/dev/null 2>&1 &' 2>/dev/null ||
        ssh -o BatchMode=yes root@proxmox0 "python3 /root/t48s/sercmd.py back-to-yealink 1" >/dev/null
    for i in $(seq 1 30); do
        ssh -o BatchMode=yes -o ConnectTimeout=5 root@$PHONE 'grep -q dropbear /tmp/custom-start.log' 2>/dev/null && { echo "Yealink back with SSH"; exit 0; }
        sleep 5
    done
    echo "Yealink did not come back with SSH" >&2; exit 1 ;;
*) echo "usage: $0 boot [IMAGE] | cmd COMMAND [SECONDS] | ssh [COMMAND] | back" >&2; exit 2 ;;
esac
