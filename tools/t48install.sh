#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# t48install.sh: put our firmware into the phone's flash, and take it out again
# (issues 8, 29, 30; docs/install.md).
#
#   t48install.sh set OUT kernel=T48S-kernel.uimg system=system.img tools=DIR
#                                     no phone: check the files and make the install set
#                                     (DIR holds the ARM programs nandrestore and nandread)
#   t48install.sh check   SET BACKUP  reads only: is everything as an install needs it?
#   t48install.sh dry-run SET BACKUP  check, then copy the set into the phone's RAM and
#                                     have the phone work out what it would write and
#                                     compare what it holds. Nothing is written
#   t48install.sh install SET BACKUP  write the kernel partition and the app partition
#   t48install.sh restore BACKUP      put Yealink's kernel and app partitions back
#
# SET is the folder "set" makes. BACKUP is the folder tools/nandbackup.sh take made
# from this phone before anything was written to it.
#
# What is written: the kernel partition (2.5 MB) and the app partition (75 MB), and
# nothing else, ever. The kernel driver cannot write anything else, and only writes
# those two while its lock is open; this script is what opens it.
#
# install and restore ask for the word to be typed. T48_YES=install (or restore)
# answers for a script. --from-live allows a phone started through recovery mode,
# which otherwise promises to write nothing. --other-phone accepts a backup whose
# checksums are not the ones in docs/backup.md (those are Tom's phone).
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
PHONE=${PHONE:-10.45.0.214}
SSHO="-o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
SSH="ssh $SSHO root@$PHONE"
R=/tmp/t48install               # on the phone, in RAM
FROM_LIVE=0; OTHER=0
args=
for a in "$@"; do
    case "$a" in --from-live) FROM_LIVE=1 ;; --other-phone) OTHER=1 ;; *) args="$args $a" ;; esac
done
set -- $args
MODE=$1
say() { echo "== $*"; }
die() { echo "STOPPED: $*" >&2; exit 1; }
usage() { sed -n '5,15p' "$0" | sed 's/^# //' >&2; exit 2; }
# partitions that are never written, in the order they lie in the flash
NEVER="0:ubl 1:uboot 2:env 3:userenv 4:logo 5:smkern 6:smrfs 8:rfs 11:cfg 12:data"
# of those, the ones something other than us changes: U-Boot (userenv, on a
# recovery start) and Yealink's firmware while it runs (cfg, data)
MOVING="userenv cfg data"

# ---- no phone ----
check_backup() {
    [ -f "$BACKUP/SHA256SUMS" ] && [ -f "$BACKUP/mtd7-kernel.dataoob" ] && [ -f "$BACKUP/mtd10-app.dataoob" ] ||
        die "$BACKUP is not a backup made by tools/nandbackup.sh take"
    ( cd "$BACKUP" && sha256sum -c --quiet SHA256SUMS ) || die "the backup does not match its own checksums"
    say "backup: every file matches its checksum"
    if [ $OTHER = 0 ]; then
        for f in "$BACKUP"/mtd*.dataoob; do
            n=$(basename "$f")
            grep -q "^$(sed -n "s/  $n\$//p" "$BACKUP/SHA256SUMS")  $n\$" "$HERE/docs/backup.md" ||
                die "$n is not the backup recorded in docs/backup.md (another phone's backup: --other-phone)"
        done
        say "backup: the twelve checksums are the ones recorded in docs/backup.md"
    fi
    python3 "$HERE/tools/nandcheck.py" ecc "$BACKUP/mtd7-kernel.dataoob" "$BACKUP/mtd10-app.dataoob" > "$TMP/ecc" || die "the backup fails the ECC check"
    grep -q " [1-9][0-9]* differ" "$TMP/ecc" && die "the backup fails the ECC check"
    say "backup: kernel and app carry the right error-correction bytes throughout"
    python3 "$HERE/tools/t48flash.py" nboot "$BACKUP/mtd7-kernel.dataoob" > "$TMP/nboot" || die "the backup's kernel is not one U-Boot would start"
    say "backup: its kernel partition holds $(sed -n 's/.*kernel image ok: //p' "$TMP/nboot")"
}
check_set() {
    for f in T48S-kernel.uimg system.img kernel.dataoob nandrestore nandread SHA256SUMS; do
        [ -f "$SET/$f" ] || die "$SET has no $f (make it with: t48install.sh set)"
    done
    ( cd "$SET" && sha256sum -c --quiet SHA256SUMS ) || die "the install set does not match its checksums"
    python3 "$HERE/tools/t48flash.py" checkkernel "$SET/T48S-kernel.uimg" > "$TMP/ck" || { cat "$TMP/ck"; die "the kernel file is not one to install"; }
    python3 "$HERE/tools/t48flash.py" kernelimage "$SET/T48S-kernel.uimg" "$TMP/kernel.dataoob" > /dev/null
    cmp -s "$TMP/kernel.dataoob" "$SET/kernel.dataoob" || die "kernel.dataoob is not what this kernel file gives"
    python3 "$HERE/tools/nandcheck.py" ecc "$SET/kernel.dataoob" > "$TMP/ecc"
    grep -q " 0 differ" "$TMP/ecc" || die "kernel.dataoob fails the ECC check"
    python3 "$HERE/tools/t48flash.py" nboot "$SET/kernel.dataoob" "$SET/T48S-kernel.uimg" > /dev/null || die "U-Boot would not read the kernel file back from kernel.dataoob"
    [ "$(head -c 4 "$SET/system.img")" = hsqs ] || die "system.img is not a system image"
    [ "$(stat -c %s "$SET/system.img")" -le $((160 * 126976)) ] || die "system.img is larger than a slot"
    say "set: kernel $(stat -c %s "$SET/T48S-kernel.uimg") bytes of 2621440, $(grep -o '[0-9]* steps match' "$TMP/ecc"); U-Boot's read of the partition image gives the file back"
    say "set: system image $(stat -c %s "$SET/system.img") bytes, sha256 $(sha256sum < "$SET/system.img" | cut -c1-16)"
}

# ---- the phone ----
state() { $SSH "sh $R/t48-flash.sh state" > "$TMP/state" 2>/dev/null || die "the phone does not answer, or has no installer files"; }
get() { sed -n "s/^$1=//p" "$TMP/state"; }
push() {
    $SSH "rm -rf $R && mkdir -p $R"
    $SSH "cat > $R/t48-flash.sh" < "$HERE/tools/phone/t48-flash.sh"
    $SSH "cat > $R/slots.sh" < "$HERE/buildroot/board/t48s/rootfs-overlay/usr/lib/t48/slots.sh"
    for t in nandrestore nandread; do
        $SSH "cat > $R/$t && chmod +x $R/$t" < "$TOOLS/$t"
        [ "$($SSH "sha256sum < $R/$t" | cut -d' ' -f1)" = "$(sha256sum < "$TOOLS/$t" | cut -d' ' -f1)" ] || die "$t did not arrive whole"
    done
}
push_set() {
    for f in kernel.dataoob system.img; do
        $SSH "cat > $R/$f" < "$SET/$f"
        [ "$($SSH "sha256sum < $R/$f" | cut -d' ' -f1)" = "$(sha256sum < "$SET/$f" | cut -d' ' -f1)" ] || die "$f did not arrive whole"
    done
    say "phone: the install set is in its RAM, checksums right"
}
# compare a partition on the phone with a file here, all 2112 bytes of every page; reads only
same() { $SSH "$R/nandrestore -c /dev/mtd$1" < "$2" > "$TMP/cmp" 2>&1; }
preflight() {
    push
    state
    case "$(get kernel)" in 6.*) ;; *) die "the phone is not running our kernel (it says: $(get kernel))" ;; esac
    [ -n "$(get mtd_kernel)" ] && [ -n "$(get mtd_app)" ] || die "the phone's kernel cannot write the kernel and app partitions (an older build)"
    [ "$(get selftest)" -gt 0 ] 2>/dev/null || die "the flash driver's self-test did not run"
    say "phone: Linux $(get kernel), started by $(b=$(get boot); echo "${b:-kexec}"), driver self-test $(get selftest) refusals, lock $(get lock)"
    if [ "$(get boot)" = live ] && [ $FROM_LIVE = 0 ]; then
        die "the phone was started through recovery mode, which promises to write nothing. To install from it anyway: --from-live"
    fi
    [ "$(get write_errors)" = 0 ] || die "the flash driver has counted write errors this session; restart the phone first"
    $SSH "cat /proc/mtd" > "$TMP/mtd"
    for p in $NEVER 7:kernel 10:app; do
        n=${p%%:*}; name=${p#*:}
        grep -q "^mtd$n: [0-9a-f]* 00020000 \"$name\"" "$TMP/mtd" || die "partition $n ($name) is not where it should be"
    done
    bad=$($SSH "$R/nandread -b /dev/mtd7 2>&1; $R/nandread -b /dev/mtd10 2>&1" | grep -c "^bad block" || true)
    [ "$bad" = 0 ] || die "the kernel or app partition has a bad block; this installer does not handle that yet"
    # what must never change: identical to the backup now (and again afterwards)
    for p in $NEVER; do
        n=${p%%:*}; name=${p#*:}
        if same "$n" "$BACKUP/mtd$n-$name.dataoob"; then
            echo "   $name: identical to the backup"
        else
            case " $MOVING " in
            *" $name "*) echo "   $name: differs from the backup (expected: U-Boot or Yealink's firmware writes it); kept for comparison afterwards"
                $SSH "$R/nandread -o /dev/mtd$n" > "$KEEP/before-$name.dataoob" 2>/dev/null ;;
            *) cat "$TMP/cmp"; die "$name on the phone is not what the backup holds: this is not the backup of this phone as it is now" ;;
            esac
        fi
    done
    # what we write: stock, or ours already (a run that was cut short)
    if same 7 "$BACKUP/mtd7-kernel.dataoob"; then KSTATE=stock
    elif [ -n "$SET" ] && same 7 "$SET/kernel.dataoob"; then KSTATE=ours
    else KSTATE=other; fi
    head -c $((539 * 64 * 2112)) "$BACKUP/mtd10-app.dataoob" > "$TMP/app-lower"
    if same 10 "$TMP/app-lower"; then ASTATE=stock; elif [ "$(get app)" = ubi ]; then ASTATE=ours; else ASTATE=other; fi
    rm -f "$TMP/app-lower"
    say "phone: kernel partition is $KSTATE, app partition is $ASTATE (below the /data blocks)"
    [ "$(get bootmode)" = none ] || echo "   note: U-Boot's BOOTMODE flag reads '$(get bootmode)'. Clear it before ever going back to Yealink (docs/install.md)"
}
after() {
    # nothing else moved: every never-written partition as it was before
    for p in $NEVER; do
        n=${p%%:*}; name=${p#*:}
        ref="$BACKUP/mtd$n-$name.dataoob"; [ ! -f "$KEEP/before-$name.dataoob" ] || ref="$KEEP/before-$name.dataoob"
        same "$n" "$ref" || { cat "$TMP/cmp"; die "$name CHANGED during this run. It should be impossible: stop and look"; }
    done
    say "untouched, compared byte for byte with before: ubl uboot env userenv logo smkern smrfs rfs cfg data"
    state
    [ "$(get refused)" = 0 ] || echo "   note: the driver refused $(get refused) writes during this run"
    [ "$(get write_errors)" = 0 ] || die "the driver counted $(get write_errors) write errors"
}
confirm() {
    [ "$T48_YES" = "$1" ] && return 0
    printf 'Type %s to go on: ' "$1"
    read -r ans
    [ "$ans" = "$1" ] || die "not confirmed"
}
save_data() {
    # the owner's settings, off the phone, whatever happens next
    if $SSH "[ -d /data/config ]" 2>/dev/null; then
        $SSH "tar -C /data -cf - ." > "$KEEP/data.tar" && say "settings: /data saved to $KEEP/data.tar ($(stat -c %s "$KEEP/data.tar") bytes)"
    fi
}

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
case "$MODE" in
set)
    [ $# -ge 2 ] || usage
    OUT=$2; shift 2
    for a in "$@"; do eval "IN_${a%%=*}=\${a#*=}"; done
    [ -f "$IN_kernel" ] && [ -f "$IN_system" ] && [ -f "$IN_tools/nandrestore" ] && [ -f "$IN_tools/nandread" ] || usage
    mkdir -p "$OUT"
    python3 "$HERE/tools/t48flash.py" checkkernel "$IN_kernel"
    cp "$IN_kernel" "$OUT/T48S-kernel.uimg"; cp "$IN_system" "$OUT/system.img"; cp "$IN_tools/nandrestore" "$IN_tools/nandread" "$OUT/"
    python3 "$HERE/tools/t48flash.py" kernelimage "$OUT/T48S-kernel.uimg" "$OUT/kernel.dataoob"
    ( cd "$OUT" && sha256sum T48S-kernel.uimg system.img kernel.dataoob nandrestore nandread > SHA256SUMS && cat SHA256SUMS )
    SET=$OUT; check_set ;;
check|dry-run|install)
    [ $# -eq 3 ] || usage
    SET=$2; BACKUP=$3; TOOLS=$SET
    KEEP=${T48_KEEP:-$HOME/t48s-install-$(date +%Y%m%d-%H%M%S)}; mkdir -p "$KEEP"
    check_backup
    check_set
    preflight
    [ "$KSTATE" != other ] || die "the kernel partition holds neither Yealink's kernel nor this set's: restore first"
    [ "$ASTATE" != other ] || echo "   note: the app partition is part way through an install; the app step will finish it"
    [ "$MODE" != check ] || { say "check passed: nothing was written, and the lock was not opened"; exit 0; }
    push_set
    # the phone works out what it would write, with its own copy of the ECC code
    $SSH "$R/nandrestore -n -o $R/would.dataoob < $R/kernel.dataoob" > "$TMP/n"
    [ "$($SSH "sha256sum < $R/would.dataoob; rm -f $R/would.dataoob" | cut -d' ' -f1)" = "$(sha256sum < "$SET/kernel.dataoob" | cut -d' ' -f1)" ] ||
        die "the phone and this PC do not agree on the bytes to write"
    say "phone: it would write exactly the PC's kernel image ($(sed 's/,.*//' "$TMP/n" | tail -n 1)), ECC bytes included"
    if [ "$MODE" = dry-run ]; then
        state
        say "dry run passed: nothing was written; lock $(get lock)"
        $SSH "rm -rf $R"
        exit 0
    fi
    cat <<EOT

About to write the phone at $PHONE:
  1. kernel partition, 2.5 MB: Yealink's kernel is replaced by ours
  2. app partition, 75 MB: Yealink's firmware is erased; ours goes in with /data
The way back is: t48install.sh restore $BACKUP
Do not unplug the phone until this says it is finished.
EOT
    confirm install
    save_data
    if [ "$KSTATE" = stock ]; then
        $SSH "sh $R/t48-flash.sh unlock kernel && sh $R/t48-flash.sh kernel; rc=\$?; sh $R/t48-flash.sh lock; exit \$rc" ||
            die "the kernel step failed. DO NOT RESTART THE PHONE. Run install again; if it keeps failing, run restore"
    else
        say "kernel partition already holds this set's kernel"
    fi
    $SSH "$R/nandread -o /dev/mtd7" > "$KEEP/after-kernel.dataoob" 2>/dev/null
    cmp "$KEEP/after-kernel.dataoob" "$SET/kernel.dataoob" || die "the kernel partition, read back to this PC, is not the image. DO NOT RESTART. Run install again"
    python3 "$HERE/tools/t48flash.py" nboot "$KEEP/after-kernel.dataoob" "$SET/T48S-kernel.uimg" | tail -n 1
    say "kernel partition: read back to this PC, identical to the image, and U-Boot's read of it gives the kernel file"
    $SSH "sh $R/t48-flash.sh unlock app && sh $R/t48-flash.sh app && sh $R/t48-flash.sh quiet" ||
        die "the app step failed. The phone would start into our rescue screen. Run install again"
    $SSH "$R/nandread -o /dev/mtd10" > "$KEEP/after-app.dataoob" 2>/dev/null
    python3 "$HERE/tools/nandcheck.py" ecc "$KEEP/after-app.dataoob" | tee "$TMP/ecc"
    grep -q " 0 differ" "$TMP/ecc" || die "the app partition has pages with wrong ECC bytes"
    python3 "$HERE/tools/t48flash.py" ubi "$KEEP/after-app.dataoob" system-a="$SET/system.img" || die "the app partition does not hold what was written"
    after
    say "INSTALLED. Restart the phone (reboot, or unplug it): it starts our firmware by itself."
    say "Evidence kept in $KEEP" ;;
restore)
    [ $# -eq 2 ] || usage
    SET=; BACKUP=$2
    KEEP=${T48_KEEP:-$HOME/t48s-restore-$(date +%Y%m%d-%H%M%S)}; mkdir -p "$KEEP"
    check_backup
    TOOLS=${TOOLS:-$HERE/native}
    [ -f "$TOOLS/nandrestore" ] && [ -f "$TOOLS/nandread" ] || die "set TOOLS to a folder with the ARM programs nandrestore and nandread (any install set)"
    preflight
    cat <<EOT

About to write the phone at $PHONE:
  1. app partition, 75 MB: our firmware and /data are erased; Yealink's goes back
  2. kernel partition, 2.5 MB: Yealink's kernel goes back
The settings in /data are saved to this PC first. Yealink's own settings were never touched.
Do not unplug the phone until this says it is finished.
EOT
    confirm restore
    save_data
    [ "$(get bootmode)" = none ] || echo "   NOTE: BOOTMODE is '$(get bootmode)': Yealink's firmware will reset its settings at its first start unless the flag is cleared first (docs/install.md)"
    # app first: if this is cut short, the phone still starts our kernel, which
    # comes up in rescue and can be restored again. Kernel last.
    $SSH "sh $R/t48-flash.sh quiet && sh $R/t48-flash.sh unlock app" || die "could not let go of the flash"
    $SSH "$R/nandrestore /dev/mtd$(get mtd_app)" < "$BACKUP/mtd10-app.dataoob" || die "restoring app failed. Run restore again"
    $SSH "sh $R/t48-flash.sh unlock kernel && $R/nandrestore /dev/mtd$(get mtd_kernel); rc=\$?; sh $R/t48-flash.sh lock; exit \$rc" < "$BACKUP/mtd7-kernel.dataoob" ||
        die "restoring the kernel failed. DO NOT RESTART THE PHONE. Run restore again"
    same 7 "$BACKUP/mtd7-kernel.dataoob" || die "the kernel partition is not the backup's. DO NOT RESTART. Run restore again"
    same 10 "$BACKUP/mtd10-app.dataoob" || die "the app partition is not the backup's. Run restore again"
    say "kernel and app: identical to the backup, every byte, spare bytes and ECC included"
    after
    say "RESTORED. Restart the phone: it starts Yealink's firmware."
    say "Our settings were saved in $KEEP" ;;
*) usage ;;
esac
