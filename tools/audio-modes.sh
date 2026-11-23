#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# audio-modes.sh LABEL: under Yealink's firmware, record which audio registers are set
# in the phone's current mode (docs/audio.md, daytime test 1). Read only. Put the phone
# in a mode first (idle, handset lifted, speakerphone, headset), then run this with a
# label; it appends one block to dumps/audio/modes.txt. Reads only safe registers:
# never /proc/ept/* (that corrupts Yealink's kernel) and never the FIFO pages.
set -e
PHONE=${PHONE:-10.45.0.214}; HERE=$(cd "$(dirname "$0")/.." && pwd)
DUMPS="$HERE/dumps"; [ -d "$DUMPS" ] || DUMPS="$(cd "$(git -C "$HERE" rev-parse --git-common-dir)/.." && pwd)/dumps"
mkdir -p "$DUMPS/audio"
ssh -o BatchMode=yes root@$PHONE '[ -x /tmp/devmem ]' 2>/dev/null || scp -q -O "$HERE/native/devmem" root@$PHONE:/tmp/devmem
{
    echo "=== ${1:-unlabelled} $(date -u +%FT%TZ)"
    ssh -o BatchMode=yes root@$PHONE '
        /tmp/devmem 0x30600020 24     # codec A and B: control, sidetone, digital gains
        /tmp/devmem 0x30600120 8      # analog power, mic mux and preamp, output modes
        /tmp/devmem 0x30600278 2      # codec C analog
        /tmp/devmem 0x80004000 1      # GPIO bank 0 data (bit 31 = external amplifier)
        cat /proc/amxr'
} >> "$DUMPS/audio/modes.txt"
echo "appended to $DUMPS/audio/modes.txt"
