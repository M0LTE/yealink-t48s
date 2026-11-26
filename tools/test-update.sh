#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# test-update.sh: the updater's network side on any PC, no root (issues 31 and
# 80, docs/install.md). The phone's own t48-update and slots.sh run against files
# standing in for the UBI volumes and the kernel partition, and a web server on
# this machine standing in for a release channel. What it shows: check, install
# and kernel from a channel, the checksums, what the Settings screen is told,
# every refusal, and the daily check on both channels. The flash itself and
# the start after an update are tools/test-install.sh's business.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'kill $SRV 2>/dev/null; rm -rf "$T"' EXIT
pass() { echo "ok   $*"; }
fail() { echo "FAIL $*" >&2; exit 1; }
sum() { sha256sum < "$1" | cut -d' ' -f1; }

# the phone: four volumes, a kernel partition, a lock
mkdir -p "$T/sys" "$T/dev" "$T/proc" "$T/run" "$T/tmp" "$T/data/config" "$T/data/phone" "$T/bin" "$T/chan"
n=0
for v in system-a system-b bootstate0 bootstate1; do
    mkdir "$T/sys/ubi0_$n"; echo $v > "$T/sys/ubi0_$n/name"
    echo 160 > "$T/sys/ubi0_$n/reserved_ebs"; echo 126976 > "$T/sys/ubi0_$n/usable_eb_size"
    : > "$T/dev/ubi0_$n"; n=$((n + 1))
done
printf 'mtd7: 00280000 00020000 "t48kernel"\n' > "$T/proc/mtd"
echo "console=ttyAMA0 t48boot=flash" > "$T/proc/cmdline"
cat > "$T/bin/ubiupdatevol" <<'EOS'
#!/bin/sh
cp "$2" "$1"
EOS
# the plain file, then ff to the end of the partition, as the flash reads back
cat > "$T/bin/nandrestore" <<'EOS'
#!/bin/sh
[ "$1" = -b ] || exit 2
[ "$(cat "$T48_LOCK")" = open-kernel ] || exit 1
cat > "$2"
EOS
chmod +x "$T/bin/"*
export PATH="$T/bin:$PATH" T48_SLOTS="$HERE/buildroot/board/t48s/rootfs-overlay/usr/lib/t48/slots.sh"
export T48_UBISYS="$T/sys" T48_DEV="$T/dev" T48_PROC="$T/proc" T48_RUN="$T/run" T48_TMP="$T/tmp" T48_LOCK="$T/lock"
export T48_UPDATE_CONF="$T/data/config/update" T48_UPDATE_KEEP="$T/data/phone" T48_RELEASE="$T/release" T48_CA=/dev/null T48_NO_SV=1
UPD="sh $HERE/buildroot/board/t48s/rootfs-overlay/usr/bin/t48-update"
info() { $UPD info | sed -n "s/^$1=//p"; }

# images: a system image is anything that starts hsqs; a kernel is a U-Boot image
mksys() { { printf hsqs; echo "$1"; head -c 3000 /dev/zero; } > "$2"; }
mkker() { python3 -c "
import struct,sys
body=sys.argv[1].encode()*500
sys.stdout.buffer.write(struct.pack('>7I',0x27051956,0,0,len(body),0x408000,0x408000,0)+bytes(36)+body)" "$1" > "$2"; }
publish() { # VERSION SYSTEM KERNEL
    echo "$1" > "$T/chan/VERSION"; cp "$2" "$T/chan/system.img"; cp "$3" "$T/chan/T48S-kernel.uimg"
    ( cd "$T/chan" && sha256sum system.img T48S-kernel.uimg > SHA256SUMS )
}
mksys one "$T/sys1.img"; mksys two "$T/sys2.img"; mkker k1 "$T/k1.uimg"; mkker k2 "$T/k2.uimg"

# installed: version one in slot a, kernel k1
. "$T48_SLOTS"
cp "$T/sys1.img" "$T/dev/ubi0_0"; cp "$T/k1.uimg" "$T/dev/mtd7"
state_read; state_write a - 0
printf 'slot=a\nnote=\n' > "$T/run/t48-slot"
echo "v1" > "$T/release"

PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
( cd "$T" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1 ) &
SRV=$!
for i in 1 2 3 4 5 6 7 8 9 10; do curl -fs "http://127.0.0.1:$PORT/" >/dev/null 2>&1 && break; sleep 0.3; done
URL=http://127.0.0.1:$PORT/chan

[ "$($UPD channel)" = stable ] && pass "the channel is stable until the owner says otherwise" || fail "default channel"
[ "$(info url)" = https://github.com/M0LTE/yealink-t48s/releases/latest/download ] && pass "stable is the newest release on GitHub" || fail "stable url"
$UPD channel testing >/dev/null
[ "$(info url)" = https://github.com/M0LTE/yealink-t48s/releases/download/system-latest ] && pass "testing is the rolling build of main" || fail "testing url"
if $UPD channel "ftp://x" 2>/dev/null || $UPD channel 'http://x/$(reboot)' 2>/dev/null; then fail "a bad channel was accepted"; fi
pass "a channel that is not stable, testing or a plain URL is refused"
$UPD channel "$URL/" >/dev/null
[ "$(info url)" = "$URL" ] && [ "$(info update)" = unknown ] && pass "the owner's own URL; nothing known before a check" || fail "own url"

if $UPD check 2>/dev/null; then fail "a check with nothing published passed"; fi
[ "$(info state)" = failed ] && [ "$(info message)" = "No release found" ] && pass "nothing published: No release found" || fail "empty channel"

publish v1 "$T/sys1.img" "$T/k1.uimg"
$UPD check | sed 's/^/     /'
[ "$(info latest)" = v1 ] && [ "$(info update)" = no ] && [ "$(info kernel)" = same ] && [ "$(info state)" = idle ] && pass "check: the channel holds what is running: up to date, kernel the same" || fail "check up to date"
[ "$($UPD install)" = "Up to date" ] && pass "install with nothing new does nothing" || fail "install up to date"

publish v2 "$T/sys2.img" "$T/k2.uimg"
$UPD check >/dev/null
[ "$(info latest)" = v2 ] && [ "$(info update)" = yes ] && [ "$(info kernel)" = differs ] && pass "check: v2 published: update available, kernel differs" || fail "check v2"

# a download that is not what SHA256SUMS says
cp "$T/chan/system.img" "$T/keep"; mksys evil "$T/chan/system.img"
if $UPD install 2>/dev/null; then fail "an image that does not match SHA256SUMS was installed"; fi
[ "$(info message)" = "Checksum mismatch" ] && [ ! -s "$T/dev/ubi0_1" ] && pass "an image that does not match SHA256SUMS: Checksum mismatch, nothing written" || fail "checksum"
grep -q "result=Checksum mismatch" "$T/data/phone/update-result" && pass "and that is kept as the last result" || fail "result kept"
cp "$T/keep" "$T/chan/system.img"

$UPD install | sed 's/^/     /'
state_read
[ "$S_ACTIVE $S_TRIAL $S_TRIES" = "a b 1" ] && cmp -s "$T/dev/ubi0_1" "$T/sys2.img" && pass "install: v2 downloaded, checked, in slot b, on trial" || fail "install"
[ "$(info state)" = restart ] && pass "the screen is told a restart is wanted" || fail "restart state"
cmp -s "$T/dev/mtd7" "$T/k1.uimg" && pass "install leaves the kernel alone" || fail "kernel touched by install"

# the restart: slot b on its try, then a healthy minute
rm -rf "$T/run/t48-update"; state_write a b 0; printf 'slot=b\nnote=trial\n' > "$T/run/t48-slot"; echo v2 > "$T/release"
$UPD boot
# but first with a cable in and no address (the switch's link register as
# the Ethernet driver shows it; eth0 as ifconfig does)
echo "imp ctrl 0x0c (bcast on, put back 0 times), gblmgmt 0x80, imp enable 0x1d, link 0x0102" > "$T/diag"
printf '#!/bin/sh\ncat %s 2>/dev/null\n' "$T/ifconfig" > "$T/bin/ifconfig"; chmod +x "$T/bin/ifconfig"
printf '#!/bin/sh\necho reboot >> %s\n' "$T/reboots" > "$T/bin/fake-reboot"; chmod +x "$T/bin/fake-reboot"
echo "eth0      Link encap:Ethernet  HWaddr 80:5E:C0:03:1C:05" > "$T/ifconfig"
export T48_ETH_DIAG="$T/diag" T48_REBOOT=fake-reboot T48_CONSOLE=/dev/null
! $UPD confirm 2>/dev/null && state_read && [ "$S_TRIAL" = b ] && [ ! -e "$T/reboots" ] && pass "a cable in and no address: not confirmed" || fail "confirmed with no network"
! $UPD confirm last 2>/dev/null && state_read && [ "$S_TRIAL" = b ] && [ "$(cat "$T/reboots")" = reboot ] && pass "the last try restarts, so the start goes back" || fail "no restart on the last try"
# the start after that restart: the first stage went back to a
cp "$T/data/phone/update-pending" "$T/pending.keep"
state_write a - 0; printf 'slot=a\nnote=failed=b\n' > "$T/run/t48-slot"
$UPD boot
[ "$(info result)" = "v2 had no network, went back" ] && pass "and that start says why: v2 had no network, went back" || fail "result after no network: $(info result)"
# back to slot b on trial for what follows
cp "$T/pending.keep" "$T/data/phone/update-pending"; sed -i '/^why=/d' "$T/data/phone/update-pending"
state_write a b 0; printf 'slot=b\nnote=trial\n' > "$T/run/t48-slot"; rm -f "$T/reboots"
# a slot standing in for one that did not start (not on trial): no verdict,
# or it would restart for ever while the DHCP server is down
state_write b - 0; printf 'slot=a\nnote=\n' > "$T/run/t48-slot"
$UPD confirm last >/dev/null 2>&1; state_read
[ "$S_ACTIVE $S_TRIAL" = "a -" ] && [ ! -e "$T/reboots" ] && pass "a slot standing in, no address: confirmed, no restart" || fail "standing-in slot restarted or not confirmed"
state_write a b 0; printf 'slot=b\nnote=trial\n' > "$T/run/t48-slot"
# a cable in the PC jack only (port 0) is no cable for this
echo "imp ctrl 0x0c, gblmgmt 0x80, link 0x0101" > "$T/diag"
$UPD confirm >/dev/null 2>&1; state_read; [ "$S_TRIAL" = - ] && [ ! -e "$T/reboots" ] && pass "no cable in the LAN jack (PC jack only): no verdict, confirmed as before" || fail "PC jack only: not confirmed"
state_write a b 0
echo "imp ctrl 0x0c, gblmgmt 0x80, link 0x0102" > "$T/diag"; echo "          inet addr:10.0.0.5  Bcast:10.0.0.255" >> "$T/ifconfig"
$UPD confirm >/dev/null
state_read; [ "$S_ACTIVE $S_TRIAL" = "b -" ] && [ "$(info result)" = "v2 installed" ] && pass "after the restart and a minute: slot b active, last result v2 installed" || fail "confirm"
[ "$(info update)" = unknown ] && [ -z "$(info latest)" ] && pass "what the check before the update found no longer counts once it runs" || fail "latest kept past the update: $(info update)"

# the next one does not start: back on b with the note
mksys three "$T/sys3.img"; publish v3 "$T/sys3.img" "$T/k2.uimg"
$UPD install >/dev/null
rm -rf "$T/run/t48-update"; state_read; state_write b - 0; printf 'slot=b\nnote=failed=a\n' > "$T/run/t48-slot"
$UPD boot
[ "$(info result)" = "v3 did not start, went back" ] && pass "an update that did not start: v3 did not start, went back" || fail "failed note: $(info result)"
printf 'slot=b\nnote=\n' > "$T/run/t48-slot"
# or does not even mount: the first stage starts the active slot in the same start
$UPD install >/dev/null
rm -rf "$T/run/t48-update"; state_read; state_write b a 0; printf 'slot=b\nnote=trial\n' > "$T/run/t48-slot"
$UPD boot
state_read
[ "$S_ACTIVE $S_TRIAL" = "b -" ] && [ "$(info result)" = "v3 did not start, went back" ] && grep -q "note=failed=a" "$T/run/t48-slot" && pass "an update that does not mount: the trial is dropped and noted in the same start" || fail "same-start fallback"
$UPD confirm >/dev/null; state_read; [ "$S_ACTIVE $S_TRIAL" = "b -" ] && pass "and the slot that stood in stays the active one" || fail "confirm after same-start fallback"
printf 'slot=b\nnote=\n' > "$T/run/t48-slot"
$UPD channel http://127.0.0.1:1/none >/dev/null
if $UPD check 2>/dev/null; then fail "check against nothing passed"; fi
[ "$(info result)" = "v3 did not start, went back" ] && pass "a check that fails does not replace the last result" || fail "check result"
$UPD channel "$URL" >/dev/null

# the kernel
if $UPD kernel "$T/k2.uimg" 0000 2>/dev/null; then fail "a kernel with the wrong checksum was written"; fi
head -c 1000 "$T/k2.uimg" > "$T/short.uimg"
if $UPD kernel "$T/short.uimg" "$(sum "$T/short.uimg")" 2>/dev/null; then fail "a kernel cut short was written"; fi
if $UPD kernel "$T/sys2.img" "$(sum "$T/sys2.img")" 2>/dev/null; then fail "a system image was written as a kernel"; fi
cmp -s "$T/dev/mtd7" "$T/k1.uimg" && pass "kernel: wrong checksum, cut short, not a kernel: all refused, partition untouched" || fail "kernel refusals"
$UPD kernel | sed 's/^/     /'
cmp -s "$T/dev/mtd7" "$T/k2.uimg" && [ "$(cat "$T/lock")" = close-kernel ] && pass "kernel from the channel: downloaded, checked, written, read back, lock closed" || fail "kernel write"
$UPD check >/dev/null; [ "$(info kernel)" = same ] && pass "check: kernel the same" || fail "kernel same"
[ "$($UPD kernel)" = "Kernel up to date" ] && pass "kernel with nothing new does nothing" || fail "kernel up to date"
echo "console=ttyAMA0 t48boot=live" > "$T/proc/cmdline"
if $UPD kernel "$T/k1.uimg" "$(sum "$T/k1.uimg")" 2>/dev/null; then fail "a kernel was written on a live start"; fi
pass "started without installing: the kernel is refused"
echo "console=ttyAMA0 t48boot=flash" > "$T/proc/cmdline"

# two at once
mkdir -p "$T/run/t48-update/lock"; echo $$ > "$T/run/t48-update/lock/pid"
if $UPD check 2>/dev/null; then fail "two updates ran at once"; fi
pass "a second run while one is going is refused"
echo 999999 > "$T/run/t48-update/lock/pid"
$UPD check >/dev/null && pass "a lock left by a run that died is taken over" || fail "stale lock"

# ---- the daily check (issue 80) ----
# the two channels, laid out as GitHub serves them: stable v4, testing v5
mksys four "$T/sys4.img"; mksys five "$T/sys5.img"
mkdir -p "$T/gh/latest/download" "$T/gh/download/system-latest"
publish v4 "$T/sys4.img" "$T/k2.uimg"; cp "$T/chan/"* "$T/gh/latest/download/"
publish v5 "$T/sys5.img" "$T/k2.uimg"; cp "$T/chan/"* "$T/gh/download/system-latest/"
export T48_UPDATE_GITHUB="http://127.0.0.1:$PORT/gh" T48_APP="$T/bin/t48-app" TZ=UTC0
printf '#!/bin/sh\n[ "$1" = check ] || exit 2\necho "2 updates"\n' > "$T/bin/t48-app"; chmod +x "$T/bin/t48-app"
before_a=$(sum "$T/dev/ubi0_0"); before_b=$(sum "$T/dev/ubi0_1")
$UPD channel stable >/dev/null
line=$($UPD daily now)
[ "$line" = "daily check: firmware v4 available, 2 app updates" ] && pass "daily check, stable: the newest release and the apps, one line: $line" || fail "daily stable: $line"
[ "$(info latest)" = v4 ] && [ "$(info update)" = yes ] && [ "$(info state)" = idle ] && pass "and the screen is told: v4, update available" || fail "daily stable info"
[ "$(sed -n 's/^last=//p' "$T/data/phone/update-check")" -gt 0 ] && pass "the time of the check is kept" || fail "last not kept"
$UPD channel testing >/dev/null
[ "$($UPD daily now)" = "daily check: firmware v5 available, 2 app updates" ] && [ "$(info latest)" = v5 ] && pass "daily check, testing: the rolling build" || fail "daily testing"
[ "$(sum "$T/dev/ubi0_0")" = "$before_a" ] && [ "$(sum "$T/dev/ubi0_1")" = "$before_b" ] && cmp -s "$T/dev/mtd7" "$T/k2.uimg" && [ ! -e "$T/data/phone/update-pending" ] && pass "it installs nothing: both slots and the kernel as they were" || fail "the daily check wrote something"
grep -q '^running=v2$' "$T/data/phone/update-latest" && pass "what it found is kept in /data, with the version it was compared with" || fail "update-latest"
# several feeds (issue 81): one not reached is said on the same line, the others still counted
printf '#!/bin/sh\n[ "$1" = check ] || exit 2\necho "Up to date, 1 feed not reached"\n' > "$T/bin/t48-app"
line=$($UPD daily now)
[ "$line" = "daily check: firmware v5 available, apps up to date, 1 feed not reached" ] && pass "one feed of several not reached: $line" || fail "daily feeds: $line"
printf '#!/bin/sh\n[ "$1" = check ] || exit 2\necho "1 update, 2 feeds not reached"\n' > "$T/bin/t48-app"
line=$($UPD daily now)
[ "$line" = "daily check: firmware v5 available, 1 app update, 2 feeds not reached" ] && pass "an update, two feeds not reached: $line" || fail "daily feeds: $line"
rm -rf "$T/run/t48-update"
[ "$(info latest)" = v5 ] && [ "$(info update)" = yes ] && pass "after a restart on the same version it still counts" || fail "latest lost at restart"
# quiet when it fails: no status, no result, what was found stays
printf '#!/bin/sh\necho "t48-app: cannot reach the server" >&2\nexit 1\n' > "$T/bin/t48-app"
mv "$T/gh" "$T/gh.away"
was=$(info result)
line=$($UPD daily now)
[ "$line" = "daily check: firmware not reached, apps not reached (cannot reach the server)" ] && pass "nothing reached: one line, $line" || fail "daily failure: $line"
[ "$(info state)" = idle ] && [ "$(info latest)" = v5 ] && [ "$(info result)" = "$was" ] && pass "and quiet: no failure shown, the last find and the last result kept" || fail "daily failure is not quiet: $(info state) $(info latest)"
if $UPD check --quiet 2>/dev/null; then fail "a quiet check against nothing passed"; fi
[ "$(info state)" = idle ] && [ "$(info latest)" = v5 ] && pass "check --quiet: the same" || fail "check --quiet"
mv "$T/gh.away" "$T/gh"
# it leaves the state alone: a kernel written and waiting for its restart, the words of a failed install
for st in 'state=restart\nmessage=' 'state=failed\nmessage=Checksum mismatch'; do
    printf "$st\n" > "$T/run/t48-update/state"; cp "$T/run/t48-update/state" "$T/state.before"
    $UPD daily now >/dev/null
    cmp -s "$T/run/t48-update/state" "$T/state.before" || fail "the daily check changed the state: $(cat "$T/run/t48-update/state")"
done
[ "$(info state)" = failed ] && [ "$(info message)" = "Checksum mismatch" ] && pass "the daily check leaves Restart to finish and a failed install's words as they were" || fail "state after daily"
printf 'state=idle\nmessage=\n' > "$T/run/t48-update/state"

# when it is due: soon after a start when the last check is over a day old, and at this phone's time in the night
due() { echo "${3:-200}.5 1.0" > "$T/proc/uptime"; T48_NOW=$1 $UPD daily due; }
D0=1795392000                             # 2026-11-23 00:00 UTC
printf 'slot=1800\nlast=\n' > "$T/data/phone/update-check"      # 01:30
[ "$(due $((D0 + 50000)))" = yes ] && pass "never checked: due once started" || fail "never checked"
[ "$(due $((D0 + 50000)) x 100)" = no ] && pass "not in the first 3 minutes after a start" || fail "start delay"
[ "$(due 1000)" = no ] && pass "not before the clock is set" || fail "clock not set"
printf 'slot=1800\nlast=%s\n' $((D0 - 86400 + 50000)) > "$T/data/phone/update-check"
[ "$(due $((D0 + 50000)))" = yes ] && pass "last check over a day ago: due" || fail "over a day"
printf 'slot=1800\nlast=%s\n' $((D0 - 3 * 3600)) > "$T/data/phone/update-check"      # 21:00 the day before
[ "$(due $((D0 + 5000)))" = no ] && [ "$(due $((D0 + 5500)))" = yes ] && pass "due at its time in the night, 01:30, not before" || fail "night slot"
printf 'slot=1800\nlast=%s\n' $((D0 + 5500)) > "$T/data/phone/update-check"
[ "$(due $((D0 + 6000)))" = no ] && pass "and once a night" || fail "twice a night"
printf 'slot=1800\nlast=%s\n' $((D0 - 3 * 3600)) > "$T/data/phone/update-check"
[ "$(due $((D0 + 7 * 3600)))" = no ] && pass "a night missed is not made up after 06:00 until a day has passed" || fail "after the night"
printf 'channel=testing\ndaily=off\n' > "$T/data/config/update"
[ "$(due $((D0 + 50000)) x 100000)" = no ] && pass "daily=off: never due" || fail "daily off"
printf 'channel=testing\n' > "$T/data/config/update"
rm "$T/data/phone/update-check"
due $((D0 + 50000)) >/dev/null
s=$(sed -n 's/^slot=//p' "$T/data/phone/update-check")
[ "$s" -ge 0 ] && [ "$s" -lt 14400 ] && pass "this phone's time is chosen at random between 01:00 and 05:00, and kept: slot $s" || fail "slot $s"
echo "all updater tests passed"
