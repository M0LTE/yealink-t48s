# SPDX-License-Identifier: AGPL-3.0-or-later
# slots.sh: the two system slots in flash and the note that says which one
# starts (issues 29 and 31, docs/flash.md). Sourced by the first stage, by
# t48-update and by the installer, so there is one copy of the rules.
#
# In UBI on the app partition:
#   system-a, system-b      static volumes, each a whole system image (squashfs).
#                           UBI keeps a checksum over every block of a static
#                           volume and refuses to read one that was half written
#   bootstate0, bootstate1  static volumes, one line each:
#                               t48boot 1 seq=N active=a|b trial=a|b|- tries=0|1
#                           Two copies, written in turn; the valid one with the
#                           highest seq counts, so a write cut short by a power
#                           loss leaves the other copy in force
#   data                    /data (UBIFS)
#
# active is the slot known to work. trial is a slot that has been written and
# not yet proved: it is started once (tries goes from 1 to 0 BEFORE it is
# started), and the running system makes it active when it has been up and well
# for a minute (t48-update confirm). If that never happens, the next start
# finds trial set with no tries left, drops it, and starts active again.
T48_UBI=${T48_UBI:-0}
T48_UBISYS=${T48_UBISYS:-/sys/class/ubi}
T48_SLOT_LEBS=160        # 20 MB a slot

# slot_vol NAME: the device of the volume with that name
slot_vol() {
    for _d in "$T48_UBISYS"/ubi"${T48_UBI}"_*; do
        if [ "$(cat "$_d/name" 2>/dev/null)" = "$1" ]; then
            echo "/dev/${_d##*/}"
            return 0
        fi
    done
    return 1
}

slot_other() { if [ "$1" = a ]; then echo b; else echo a; fi; }

# state_parse LINE: P_SEQ P_ACTIVE P_TRIAL P_TRIES, or fails
state_parse() {
    set -- $1
    [ $# = 6 ] && [ "$1" = t48boot ] && [ "$2" = 1 ] || return 1
    case "$3$4$5$6" in seq=*active=*trial=*tries=*) ;; *) return 1 ;; esac
    P_SEQ=${3#seq=}; P_ACTIVE=${4#active=}; P_TRIAL=${5#trial=}; P_TRIES=${6#tries=}
    case "$P_SEQ" in ''|*[!0-9]*|??????????*) return 1 ;; esac
    case "$P_ACTIVE" in a|b) ;; *) return 1 ;; esac
    case "$P_TRIAL" in a|b|-) ;; *) return 1 ;; esac
    case "$P_TRIES" in 0|1) ;; *) return 1 ;; esac
    [ "$P_TRIAL" != "$P_ACTIVE" ] || return 1
}

# state_read: S_SEQ S_ACTIVE S_TRIAL S_TRIES, and S_VALID=1 if a copy was readable.
# With no readable copy: slot a, no trial.
state_read() {
    S_SEQ=0; S_ACTIVE=a; S_TRIAL=-; S_TRIES=0; S_VALID=0
    for _i in 0 1; do
        _v=$(slot_vol bootstate$_i) || continue
        _l=$(head -c 200 "$_v" 2>/dev/null | head -n 1)
        state_parse "$_l" || continue
        if [ "$S_VALID" = 0 ] || [ "$P_SEQ" -gt "$S_SEQ" ]; then
            S_SEQ=$P_SEQ; S_ACTIVE=$P_ACTIVE; S_TRIAL=$P_TRIAL; S_TRIES=$P_TRIES; S_VALID=1
        fi
    done
}

# state_write ACTIVE TRIAL TRIES: after a state_read. Writes the copy the current
# state is not in, reads it back, and fails if what is in force is not what was asked.
state_write() {
    _seq=$((S_SEQ + 1))
    _v=$(slot_vol bootstate$((_seq % 2))) || return 1
    _f=${T48_TMP:-/tmp}/t48-bootstate.$$
    printf 't48boot 1 seq=%d active=%s trial=%s tries=%s\n' "$_seq" "$1" "$2" "$3" > "$_f"
    ubiupdatevol "$_v" "$_f" >/dev/null 2>&1
    _rc=$?
    rm -f "$_f"
    _a=$1; _t=$2; _n=$3
    state_read
    [ $_rc = 0 ] && [ "$S_SEQ" = "$_seq" ] && [ "$S_ACTIVE" = "$_a" ] && [ "$S_TRIAL" = "$_t" ] && [ "$S_TRIES" = "$_n" ]
}

# slot_plan: decide what to start, and count the try. Sets SLOT_ORDER (slots to
# try, in order) and SLOT_NOTE (trial, failed=X, or nothing).
slot_plan() {
    state_read
    SLOT_NOTE=
    SLOT_ORDER="$S_ACTIVE $(slot_other "$S_ACTIVE")"
    [ "$S_TRIAL" != - ] || return 0
    if [ "$S_TRIES" -gt 0 ]; then
        _trial=$S_TRIAL
        # the try is used up before the slot is started; if that cannot be
        # written down, the slot is not started
        if state_write "$S_ACTIVE" "$_trial" 0; then
            SLOT_ORDER="$_trial $S_ACTIVE"
            SLOT_NOTE=trial
        fi
    else
        SLOT_NOTE="failed=$S_TRIAL"
        state_write "$S_ACTIVE" - 0
        SLOT_ORDER="$S_ACTIVE $(slot_other "$S_ACTIVE")"
    fi
}

# slot_load SLOT FILE: copy the slot's system image out of flash
slot_load() {
    _v=$(slot_vol "system-$1") || return 1
    cat "$_v" > "$2" 2>/dev/null && [ -s "$2" ]
}

# slot_sum SLOT: the SHA-256 of what the slot holds; nothing for an empty or unreadable one
slot_sum() {
    _v=$(slot_vol "system-$1") || return 1
    _s=$(sha256sum < "$_v" 2>/dev/null | cut -d' ' -f1)
    # an empty slot has nothing in it to name
    [ "$_s" != e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 ] || _s=
    echo "$_s"
}
