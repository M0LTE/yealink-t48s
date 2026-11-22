#!/bin/sh
# kexec-boot.sh IMAGE [WDT]: boot IMAGE (raw kernel Image) on the phone from Yealink's
# running kernel via kmod/t48kexec.ko, with no key presses. The watchdog (WDT seconds,
# default 90) resets the phone back to Yealink if the new kernel does not disarm it.
# Output appears on the serial console (logger on proxmox0).
set -e
IMG=$1; WDT=${2:-90}; PHONE=${PHONE:-10.45.0.214}
HERE=$(cd "$(dirname "$0")/.." && pwd)
# dumps/ is gitignored, so a git worktree has none: use the main checkout's
DUMPS="$HERE/dumps"; [ -f "$DUMPS/t48kexec.ko" ] || DUMPS="$(cd "$(git -C "$HERE" rev-parse --git-common-dir)/.." && pwd)/dumps"
# The boot hook disarms itself at each boot and re-arms 2 minutes later. If we jump
# away before then, the next Yealink boot has no hook and no SSH. SSH being up proves
# this boot is good, so re-arm it now, and refuse to go on without it.
ssh -o BatchMode=yes root@$PHONE '[ -f /config/simulator.sh.armed ] && mv /config/simulator.sh.armed /config/simulator.sh; sync; [ -x /config/simulator.sh ]' || { echo "boot hook not in place; not jumping" >&2; exit 1; }
scp -q -O "$IMG" root@$PHONE:/tmp/Image
scp -q -O "$DUMPS/t48kexec.ko" root@$PHONE:/tmp/t48kexec.ko
# freeze Yealink's UI (it keeps re-arming the keypad chip's watchdog), turn that
# watchdog off (Yealink's scripts do this too), network down (no DMA into
# memory after the jump), then load; detached because the network goes away
ssh -o BatchMode=yes root@$PHONE "sh -c 'trap \"\" HUP; killall -STOP dskPhone.exx; echo 0 > /proc/keypad/watchdog; sleep 1; ifconfig eth0 down; insmod /tmp/t48kexec.ko image=/tmp/Image wdt=$WDT' > /tmp/kexec.log 2>&1 < /dev/null &" || true
echo "loader started on $PHONE"
