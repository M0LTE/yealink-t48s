#!/bin/sh
# ourkernel.sh boot [IMAGE]   : start our kernel from Yealink and wait for its shell prompt
# ourkernel.sh cmd "COMMAND" [SECONDS] : run a command in its serial shell, print the output
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
back)
    ssh -o BatchMode=yes root@proxmox0 "python3 /root/t48s/sercmd.py back-to-yealink 1" >/dev/null
    for i in $(seq 1 30); do
        ssh -o BatchMode=yes -o ConnectTimeout=5 root@$PHONE 'grep -q dropbear /tmp/custom-start.log' 2>/dev/null && { echo "Yealink back with SSH"; exit 0; }
        sleep 5
    done
    echo "Yealink did not come back with SSH" >&2; exit 1 ;;
*) echo "usage: $0 boot [IMAGE] | cmd COMMAND [SECONDS] | back" >&2; exit 2 ;;
esac
