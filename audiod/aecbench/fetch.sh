#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Fetches what the echo canceller comparison needs (issue #35) into DIR
# (default build/src): the two candidates' sources and some free speech.
# Nothing fetched here is committed; what audiod ships is in ../third_party.
#   speexdsp          BSD-3-Clause, xiph.org, pinned commit
#   WebRTC AECM       BSD-3-Clause plus a patent grant, from the
#                     webrtc-audio-processing 0.3.1 release (the last with
#                     the mobile canceller in plain C)
#   CMU Arctic        two speakers, 16 kHz, free for any use (see its COPYING)
set -e
D=${1:-build/src}
mkdir -p "$D/speech"
cd "$D"
if [ ! -d speexdsp ]; then
	git clone -q https://github.com/xiph/speexdsp.git
	git -C speexdsp checkout -q 7a158783df74efe7c2d1c6ee8363c1e695c71226
fi
if [ ! -d wap ]; then
	curl -sfL -o wap.tar.xz https://freedesktop.org/software/pulseaudio/webrtc-audio-processing/webrtc-audio-processing-0.3.1.tar.xz
	echo "a0fdd938fd85272d67e81572c5a4d9e200a0c104753cb3c209ded175ce3c5dbf  wap.tar.xz" | sha256sum -c -
	mkdir wap && tar -C wap --strip-components=1 -xf wap.tar.xz
fi
for s in bdl slt; do
	for n in 01 02 03 04 05 06 07 08 09 10 11 12; do
		[ -s speech/${s}_00$n.wav ] || curl -sfL -o speech/${s}_00$n.wav \
			http://festvox.org/cmu_arctic/cmu_arctic/cmu_us_${s}_arctic/wav/arctic_a00$n.wav
	done
done
echo "sources in $D"
