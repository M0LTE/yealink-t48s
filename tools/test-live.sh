#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# test-live.sh: run the live image's boot step (t48-live) on this PC against a
# stand-in for the kernel's /sys/kernel/t48s_bootmode, for every case it must handle.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
LIVE=$HERE/buildroot/board/t48s/rootfs-overlay/usr/bin/t48-live
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail=0
# case NAME CMDLINE MODE WRITER FROM NOW "text the second line must hold"
#   WRITER: works (a write clears the flag), readonly (it does not), none (no kernel support)
case_() {
    name=$1; rm -rf "$T/bm" "$T/run"; mkdir -p "$T/run"
    echo "$2" > "$T/cmdline"
    if [ "$4" != none ]; then
        mkdir -p "$T/bm"; echo "$3" > "$T/bm/mode"; echo "not tried" > "$T/bm/status"
        # play the kernel's part: "clear" is a pipe, and whoever reads the word acts on it
        mkfifo "$T/bm/clear"
        timeout 1 sh -c 'read w < "$0/clear" && : > "$0/asked" && [ "$1" = works ] && [ "$w" = BOOTMODE ] && echo none > "$0/mode" && echo "BOOTMODE cleared" > "$0/status"' "$T/bm" "$4" &
        sleep 0.1
    fi
    T48_CMDLINE=$T/cmdline T48_BOOTMODE=$T/bm T48_RUN=$T/run sh "$LIVE" boot > "$T/out" 2>&1 || true
    wait
    if [ "$5" = - ]; then
        if [ -e "$T/run/t48-notice" ] || [ -e "$T/run/t48-live" ] || [ -e "$T/bm/asked" ]; then echo "FAIL $name: acted on a boot that is not live"; fail=1; else echo "ok   $name: left alone"; fi
        return
    fi
    if grep -qx "from=$5" "$T/run/t48-live" && grep -qx "bootmode_now=$6" "$T/run/t48-live" \
       && [ "$(wc -l < "$T/run/t48-notice")" = 2 ] && sed -n 2p "$T/run/t48-notice" | grep -q "$7" \
       && grep -q "Nothing is installed" "$T/run/t48-notice"; then
        echo "ok   $name: $(sed -n 2p "$T/run/t48-notice")"
    else
        echo "FAIL $name"; cat "$T/run/t48-live" "$T/run/t48-notice" "$T/out" 2>/dev/null; fail=1
    fi
}
case_ "kexec boot"            "console=tty0"              usbmode    works    -        -          -
case_ "usb, flag cleared"     "x t48boot=live loglevel=8" usbmode    works    usb      none       "take the stick out"
case_ "usb, read-only kernel" "x t48boot=live loglevel=8" usbmode    readonly usb      usbmode    "take the stick out"
case_ "tftp, flag cleared"    "x t48boot=live"            resumemode works    network  none       "plug it in again"
case_ "tftp, read-only"       "x t48boot=live"            resumemode readonly network  resumemode "settings will be reset"
case_ "live, no flag"         "t48boot=live"              none       readonly recovery none       "plug it in again"
case_ "live, old kernel"      "t48boot=live"              -          none     recovery unreadable "plug it in again"
case_ "not quite the word"    "t48boot=liver"             usbmode    works    -        -          -
[ $fail = 0 ] && echo "t48-live: all cases pass"
exit $fail
