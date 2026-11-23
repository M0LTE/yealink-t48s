#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# led-test.sh [SECONDS]: light each lamp of the phone in turn so someone can watch
# (issue #3). Needs our kernel and SSH. Each lamp is on for SECONDS (default 4), then
# the power lamp blinks for as long again. Everything is off at the end. Silent.
# The shell only touches a lamp when its state changes, so it does not get in the way
# as long as nobody uses the phone meanwhile.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
S=${1:-4}
p() { sh "$HERE/ourkernel.sh" ssh "$1"; }
L=/sys/class/leds
step() {    # LED, what to look for
    echo "$2"
    p "echo none > $L/$1/trigger; echo 1 > $L/$1/brightness; sleep $S; echo 0 > $L/$1/brightness"
}
step t48s:red:power     "1. power lamp, top right of the case: on, red"
step t48s:green:headset "2. Headset key: on, green"
step t48s:red:mute      "3. Mute key: on, red"
step t48s:green:speaker "4. Speakerphone key: on, green"
echo "5. power lamp: blinking, about three times in two seconds"
p "echo timer > $L/t48s:red:power/trigger; echo 300 > $L/t48s:red:power/delay_on; echo 300 > $L/t48s:red:power/delay_off; sleep $S; echo none > $L/t48s:red:power/trigger; echo 0 > $L/t48s:red:power/brightness"
echo "6. all four: on together"
p "for l in $L/t48s:*; do echo 1 > \$l/brightness; done; sleep $S; for l in $L/t48s:*; do echo 0 > \$l/brightness; done"
echo "done: all lamps off"
