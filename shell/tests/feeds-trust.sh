#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# feeds-trust.sh BUILD_DIR: what an added feed may not do (issue 81, docs/apps.md#feeds).
# An installed app updates only from the feed it came from; one with no record (built in,
# from a file, from before issue 81) from the official feed only, and one from before
# issue 81 also from the old single feed setting. A feed offering the same id newer, the
# built-in clock included, changes nothing, with the official feed up or down. The
# official feed is recorded as "official", not its address. Run by make check.
set -e
cd "$1"
B=$PWD
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass() { echo "ok   $1"; }
fail() { echo "FAIL $1"; exit 1; }
official() { rm -f "$T/off"; ln -s "$1" "$T/off"; }
app() { T48_OFFICIAL_FEED="file://$T/off/index.json" T48_FEED_TIMEOUT=2 ./t48-app --apps "$T/inst:apps" --config "$T/config" \
    --data "$T/data" --state "$T/state" "$@"; }
row() { app list 2>/dev/null | grep "^$1 "; }
cfg() { ./t48-config --config "$T/config" "$@"; }
mkdir -p "$T/inst" "$T/config"

official "$B/feeds/3"                                   # demo and memo 1.0.0
cfg set feed url1 "file://$B/feeds/hostile/index.json"  # clock 9.9.9, memo 2.0.0
app install memo >/dev/null
grep -qx "memo official" "$T/state/app-sources" && pass "installed from the official feed: recorded as official, not its address" || fail "record: $(cat "$T/state/app-sources")"
row memo | grep -q "^memo  *1.0.0  *1.0.0 .* installed  *Official " && pass "official up: the other feed's newer memo is not an update" || fail "memo: $(row memo)"
[ "$(app check)" = "Up to date" ] && pass "and the daily check does not count it" || fail "check: $(app check)"
row clock | grep -q "^clock  *0.1.0  *-  .* builtin  *-  " && pass "official up: a feed offering clock 9.9.9 cannot replace the built-in clock" || fail "clock: $(row clock)"
if app update clock >/dev/null 2>&1; then fail "clock updated from another feed"; fi
[ ! -e "$T/inst/clock" ] && pass "update clock: refused, nothing installed" || fail "clock installed"
app --machine list | grep -q "^app	memo	.*	Official	hostile$" && pass "the other feed is named as also having it" || fail "machine: $(app --machine list | grep memo)"
app --machine list | grep "^app	clock	" | grep -q "	-	-$" && pass "and nothing about the built-in clock" || fail "clock machine line"

official "$B/feeds/none"                                # the official feed down
row memo | grep -q "^memo  *1.0.0  *-  .* installed  *Official " && pass "official down: still no update from the other feed" || fail "memo down: $(row memo)"
row clock | grep -q " builtin " && pass "official down: clock stays built in" || fail "clock down: $(row clock)"
[ "$(app check)" = "Up to date, 1 feed not reached" ] && pass "official down: the daily check counts nothing, and says so" || fail "check down: $(app check)"
if app update memo >"$T/out" 2>&1; then fail "memo updated with the official feed down"; fi
grep -q "Official: " "$T/out" && pass "update memo: refused, naming the official feed" || fail "update memo: $(cat "$T/out")"
[ "$(app --machine list | grep -v '^progress ' | head -n 1)" = "feederror Official: not found on the server" ] && pass "--machine list: the feed errors come first" || fail "first line: $(app --machine list | head -n 3)"

# the official feed moves: an app recorded as official follows it
official "$B/feeds/4"                                   # memo 1.1.0
app update memo >/dev/null && grep -q '"version": "1.1.0"' "$T/inst/memo/manifest.json" && pass "official feed at a new address: memo updates from it" || fail "moved official"

# no record, as an app installed before issue 81: the official feed and the old url setting, never another
sed -i '/^memo /d' "$T/state/app-sources"
official "$B/feeds/3"
row memo | grep -q " installed  *Official " && pass "no record: follows the official feed" || fail "norecord: $(row memo)"
cfg set feed url "file://$B/feeds/2/index.json"         # the old setting: demo 1.1.0 only
cfg set feed url1 "file://$B/feeds/hostile/index.json"
row memo | grep -q "^memo  *1.1.0  *1.0.0 " && pass "no record: not the other feed's 2.0.0" || fail "norecord hostile: $(row memo)"
app install demo >/dev/null 2>&1 && sed -i '/^demo /d' "$T/state/app-sources"
row demo | grep -q "^demo  *1.0.0  *1.1.0  .* update " && pass "no record: the old url setting's newer demo is an update, as before" || fail "legacy: $(row demo)"

# from a file: the official feed only, never the old setting
app install "$B/feeds/2/demo-1.1.0.t48app" >/dev/null
grep -qx "demo file" "$T/state/app-sources" && pass "from a file: recorded as file" || fail "file record"
cfg unset feed url
app install "$B/feeds/hostile/memo-2.0.0.t48app" >/dev/null
row memo | grep -q "^memo  *2.0.0  *1.0.0  .* installed " && pass "from a file: follows the official feed only" || fail "file memo: $(row memo)"

# a feed cannot take the official feed's name
mkdir -p "$T/Official" && cp "$B/feeds/hostile/"* "$T/Official/"
cfg set feed url2 "file://$T/Official/index.json"
app --machine list | grep -q "	Official	hostile,Official-2$" && pass "a feed named Official is shown as Official-2" || fail "names: $(app --machine list | grep memo)"
echo "all feed trust tests passed"
