#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# test-pack.sh: the SDK's t48-pack on any PC (sdk/README.md). An app folder is
# packed and indexed; the manifest's category (issue 30) must be one of the fixed
# set, goes into index.json, and is General when the manifest has none.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
PACK="python3 $HERE/sdk/t48-pack"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass() { echo "ok   $*"; }
fail() { echo "FAIL $*" >&2; exit 1; }

# an app folder: id, then the manifest's category line (empty: none)
app() {
    mkdir -p "$T/src/$1"
    printf '\177ELF' > "$T/src/$1/$1"
    chmod +x "$T/src/$1/$1"
    printf '{ "id": "%s", "name": "%s", "version": "1.0.0", "api": 1, "exec": "%s",%s "permissions": [] }\n' "$1" "$1" "$1" "$2" > "$T/src/$1/manifest.json"
}
cat_of() { python3 -c 'import json, sys; print({a["id"]: a.get("category") for a in json.load(open(sys.argv[1]))["apps"]}[sys.argv[2]])' "$T/feed/index.json" "$1"; }

app radio ' "category": "Radio",'
app plain ''
$PACK "$T/src/radio" "$T/feed" >/dev/null 2>&1 || fail "a manifest with a category is packed"
$PACK "$T/src/plain" "$T/feed" >/dev/null 2>&1 || fail "a manifest without one is packed"
pass "packed with and without a category"

for c in General Games Radio Tools Media; do
    id=$(echo "$c" | tr A-Z a-z)
    app "$id" " \"category\": \"$c\","
    $PACK "$T/src/$id" "$T/out" >/dev/null 2>&1 || fail "$c is refused"
done
pass "every category in the set is taken"

app bad ' "category": "Weather",'
if $PACK "$T/src/bad" "$T/feed" >/dev/null 2>"$T/err"; then fail "an unknown category is packed"; fi
grep -q "category must be one of General, Games, Radio, Tools, Media" "$T/err" || fail "the refusal says why: $(cat "$T/err")"
app lower ' "category": "radio",'
if $PACK "$T/src/lower" "$T/feed" >/dev/null 2>&1; then fail "a category in the wrong case is packed"; fi
pass "an unknown category is refused, with the list"

$PACK --index "$T/feed" >/dev/null
[ "$(cat_of radio)" = Radio ] || fail "the index carries the category: $(cat_of radio)"
[ "$(cat_of plain)" = General ] || fail "no category is General in the index: $(cat_of plain)"
[ ! -e "$T/feed/bad-1.0.0.t48app" ] || fail "a refused app left a package"
pass "index.json carries the category, General when there is none"
echo "all t48-pack tests passed"
