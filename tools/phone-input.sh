#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# phone-input.sh: press keys, touch the screen and lift the handset on the real phone
# without being there, and take a picture of its screen. Needs our kernel (the "inject"
# test hooks in t48s_keypad and t48s_touch) and SSH. The events go through the real
# input devices, so t48-shell sees them exactly as it sees a finger.
#
#   phone-input.sh key NAME [MS]     press and release (held 60 ms, or MS)
#   phone-input.sh keydown NAME | keyup NAME
#   phone-input.sh touch X Y [MS]    tap at a screen position (pixels)
#   phone-input.sh touchdown X Y | touchup
#   phone-input.sh hook off | on     lift or replace the handset
#   phone-input.sh shot FILE.png     save the screen
#   phone-input.sh log               the shell's log
# Several commands can be chained with "--": key ok -- shot a.png
# Key names are the simulator's (docs/shell.md): 0-9 star hash up down left right ok x
# volup voldown headset mute message hold redial transfer speaker
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
p() { sh "$HERE/ourkernel.sh" ssh "$1"; }
K=/sys/module/t48s_keypad/parameters/inject
T=/sys/module/t48s_touch/parameters/inject
ms() { awk "BEGIN{printf \"%.3f\", $1/1000}"; }

keycode() {
    case "$1" in
    1) echo 2;; 2) echo 3;; 3) echo 4;; 4) echo 5;; 5) echo 6;; 6) echo 7;; 7) echo 8;; 8) echo 9;; 9) echo 10;; 0) echo 11;;
    star) echo 55;; hash) echo 523;; up) echo 103;; down) echo 108;; left) echo 105;; right) echo 106;;
    ok) echo 28;; x) echo 14;; voldown) echo 114;; volup) echo 115;; headset) echo 148;; mute) echo 113;;
    message) echo 430;; hold) echo 119;; redial) echo 129;; transfer) echo 159;; speaker) echo 149;;
    *) echo "unknown key $1" >&2; exit 2;;
    esac
}

# screen pixels to the touch chip's raw values: /etc/pointercal backwards
raw() {
    p 'cat /etc/pointercal' | awk -v x="$1" -v y="$2" '{
        det = $1 * $5 - $2 * $4
        u = x * $7 - $3; v = y * $7 - $6
        printf "%d %d", (u * $5 - $2 * v) / det, ($1 * v - $4 * u) / det }'
}

one() {
    case "$1" in
    key) c=$(keycode "$2"); p "echo $c 1 > $K; sleep $(ms "${3:-60}"); echo $c 0 > $K";;
    keydown) p "echo $(keycode "$2") 1 > $K";;
    keyup) p "echo $(keycode "$2") 0 > $K";;
    touch) r=$(raw "$2" "$3"); p "echo $r > $T; sleep $(ms "${4:-60}"); echo up > $T";;
    touchdown) p "echo $(raw "$2" "$3") > $T";;
    touchup) p "echo up > $T";;
    hook) case "$2" in off) v=1;; on) v=0;; *) echo "hook off | on" >&2; exit 2;; esac; p "echo 169 $v > $K";;
    # one read of the whole screen, so the picture is not torn by a redraw
    shot) p 'dd if=/dev/fb0 bs=1536000 count=1 2>/dev/null' | python3 -c '
import sys
from PIL import Image
d = sys.stdin.buffer.read()
Image.frombuffer("RGBA", (800, 480), d[:800 * 480 * 4], "raw", "BGRA", 0, 1).convert("RGB").save(sys.argv[1])' "$2";;
    log) p 'cat /var/log/t48-shell.log';;
    *) sed -n '3,17p' "$0" >&2; exit 2;;
    esac
}

while [ $# -gt 0 ]; do
    n=0; for a in "$@"; do [ "$a" = "--" ] && break; n=$((n + 1)); done
    # shellcheck disable=SC2046
    one $(i=0; for a in "$@"; do [ $i -lt $n ] && printf '%s ' "$a"; i=$((i + 1)); done)
    shift $n; if [ $# -gt 0 ]; then shift; fi
done
