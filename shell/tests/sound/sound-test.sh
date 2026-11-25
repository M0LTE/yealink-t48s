#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Sound from an app, end to end on this PC and without a sound: the simulator
# runs the tones app against a real t48-audiod on its file backend, and what
# the audio service "played" is read back from its WAV file.
#   sound-test.sh BUILD_DIR     (run by make check)
set -e
O=$(cd "$1" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
make -s -C "$HERE/../../../audiod" >/dev/null
AUDIOD=$HERE/../../../audiod/build/t48-audiod
RUN=$(mktemp -d)
trap 'kill $PID 2>/dev/null || true; rm -rf "$RUN"' EXIT
"$AUDIOD" --run-dir "$RUN" --spk "$RUN/spk.wav" 2>"$RUN/audiod.log" &
PID=$!
echo "== tests/sound/sound.script (with t48-audiod)"
cd "$O"
rm -rf simdata
./t48-sim --apps simdata/installed:apps --data simdata/appdata --config simdata/config --state simdata/state \
    --audio "$RUN/ctl.sock" --script ../tests/sound/sound.script 2>sim.log || { cat sim.log "$RUN/audiod.log"; exit 1; }
kill $PID; wait $PID 2>/dev/null || true
cat "$RUN/audiod.log"
python3 - "$RUN/spk.wav" <<'PY'
import math, struct, sys, wave
w = wave.open(sys.argv[1]); d = w.readframes(w.getnframes())
x = struct.unpack("<%dh" % (len(d) // 2), d)
def amp(s, hz):
    k = 2 * math.pi * hz / 16000
    return 2 * math.hypot(sum(v * math.cos(k * i) for i, v in enumerate(s)), sum(v * math.sin(k * i) for i, v in enumerate(s))) / max(len(s), 1)
def loudest(hz, n=1600):
    return max((amp(x[i:i + n], hz), i) for i in range(0, len(x) - n, n // 2))
fails = 0
def check(ok, what):
    global fails
    print(("  ok   " if ok else "  FAIL ") + what)
    fails += not ok
check(max(abs(v) for v in x) <= 1800, "nothing above the loudspeaker's limit")
# the four notes, in order, at the Apps volume a phone starts with, 12: 24000 * 902 / 32768 = 661
at = []
for hz in (523, 659, 784, 1047):
    a, i = loudest(hz)
    at.append(i)
    check(600 < a < 680, "note %d Hz played at %.0f (want about 661)" % (hz, a))
check(at == sorted(at), "the notes came in order")
# the steady tone at volume 14: 24000 * 1430 / 32768 = 1047
a, i = loudest(600, 8000)
check(1000 < a < 1070, "steady 600 Hz at %.0f after two presses of volume up (want 1047)" % a)
# the dial tone when the handset was lifted, and none of the app's tone under it
a350, j = loudest(350, 8000)
check(a350 > 300, "dial tone when the handset was lifted (350 Hz at %.0f)" % a350)
check(amp(x[j:j + 8000], 600) < 30, "no app sound under the dial tone (600 Hz at %.0f)" % amp(x[j:j + 8000], 600))
a, k = loudest(880, 800)
check(a > 600 and k > j, "the beep after the handset went down (880 Hz at %.0f)" % a)
sys.exit(1 if fails else 0)
PY
