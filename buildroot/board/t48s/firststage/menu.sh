# SPDX-License-Identifier: AGPL-3.0-or-later
# menu.sh: the rescue menu (issue 32, docs/install.md). Sourced by the first
# stage (/init) as /lib/t48-menu.sh. It is shown on an installed phone when X is
# held while the kernel starts, and when no system slot starts. Text on the
# screen console, keys through t48-keys: Up, Down and OK, or the number.
#
#   menu_main key             X was held: start, the other slot, settings wiped,
#                             rescue, restore help. Starts normally after a
#                             minute with no key (a key stuck down)
#   menu_main failed REASON   nothing starts: rescue, restart, restore help.
#                             Rescue after 30 seconds with no key, so that a
#                             phone nobody is at ends up answering SSH
# Both set MENU_CHOICE: start, other, wipe, rescue or restart.
#   data_aside                move everything in /data into /data/aside-N
MENU_TTY=${MENU_TTY:-/dev/tty1}
MENU_KEYS=${MENU_KEYS:-t48-keys}

# menu_choose HEADING DEFAULT SECONDS KEY|LABEL...   sets MENU_PICK to a KEY.
# SECONDS 0: wait for ever. Any key press stops the countdown.
menu_choose() {
    _head=$1; _def=$2; _left=$3; shift 3
    _n=$#; _sel=1; _i=1; _deflabel=
    for _it; do
        [ "${_it%%|*}" != "$_def" ] || { _sel=$_i; _deflabel=${_it#*|}; }
        _i=$((_i + 1))
    done
    _draw=1
    while :; do
        if [ $_draw = 1 ]; then
            {
                printf '\033[2J\033[H\n  T48S open firmware\n\n'
                [ -z "$_head" ] || printf '  %s\n\n' "$_head"
                _i=1
                for _it; do
                    if [ $_i = $_sel ]; then printf '  \033[7m %d  %-30s \033[0m\n' $_i "${_it#*|}"
                    else printf '   %d  %s\n' $_i "${_it#*|}"; fi
                    _i=$((_i + 1))
                done
                printf '\n  Up, Down, OK\n'
            } > "$MENU_TTY" 2>/dev/null
            _draw=0
        fi
        # the countdown has a line of its own, under the hint
        [ "$_left" -le 0 ] || printf '\033[%d;1H\033[K  %s in %d s' $((_n + 7 + (${#_head} > 0 ? 2 : 0))) "$_deflabel" "$_left" > "$MENU_TTY" 2>/dev/null
        _k=$($MENU_KEYS read 1)
        case $? in
        0)  [ "$_left" -le 0 ] || { _left=0; _draw=1; }
            case "$_k" in
            up) [ $_sel -le 1 ] || _sel=$((_sel - 1)); _draw=1 ;;
            down) [ $_sel -ge $_n ] || _sel=$((_sel + 1)); _draw=1 ;;
            [1-9]) [ "$_k" -gt $_n ] || { _sel=$_k; break; } ;;
            ok) break ;;
            esac ;;
        1)  if [ "$_left" -gt 0 ]; then
                _left=$((_left - 1))
                [ "$_left" -gt 0 ] || break
            fi ;;
        *)  break ;;        # no keypad: the default
        esac
    done
    _i=1
    for _it; do
        [ $_i != $_sel ] || MENU_PICK=${_it%%|*}
        _i=$((_i + 1))
    done
}

menu_help() {
    {
        printf '\033[2J\033[H\n  Restore help\n\n'
        echo "  Yealink's firmware goes back from the backup made before the"
        echo "  install, from a PC:"
        echo
        echo "      tools/t48install.sh restore BACKUP"
        echo
        echo "  The phone must answer SSH for that: choose Rescue."
        echo
        echo "  If the phone shows nothing but the logo: hold Speaker while"
        echo "  connecting the power, start the live image from USB or TFTP,"
        echo "  and run the restore against that."
        echo
        echo "  Settings wiped from this menu are kept in /data/aside-1,"
        echo "  aside-2 and so on. Moving the files back restores them."
        echo
        echo "  github.com/M0LTE/yealink-t48s, docs/install.md"
        echo
        echo "  OK to go back"
    } > "$MENU_TTY" 2>/dev/null
    while :; do
        _k=$($MENU_KEYS read 600)
        [ $? = 0 ] && [ "$_k" != ok ] && [ "$_k" != x ] || break
    done
}

menu_main() {
    _wait=60
    [ "$1" = key ] || _wait=30
    while :; do
        if [ "$1" = key ]; then
            menu_choose "" start $_wait "start|Start" "other|Start slot $MENU_OTHER" "wipe|Start with settings wiped" "rescue|Rescue" "help|Restore help"
        else
            menu_choose "$2" rescue $_wait "rescue|Rescue" "restart|Restart" "help|Restore help"
        fi
        _wait=0             # someone is there
        case "$MENU_PICK" in
        help) menu_help; continue ;;
        wipe)
            menu_choose "Wipe settings? Kept in /data/aside" cancel 0 "cancel|Cancel" "wipe|Wipe"
            [ "$MENU_PICK" = wipe ] || continue ;;
        esac
        MENU_CHOICE=$MENU_PICK
        printf '\033[2J\033[H' > "$MENU_TTY" 2>/dev/null
        return 0
    done
}

# data_aside: the owner's settings, keys and apps out of the way, not deleted.
# /data is the UBI volume "data"; UBI is attached. The system image makes the
# folders it needs again when it starts.
data_aside() {
    _m=${MENU_DATA_MNT:-/mnt}
    mkdir -p "$_m"
    [ -n "$MENU_DATA_MNT" ] || mount -t ubifs ubi0:data "$_m" || return 1
    _a=1
    while [ -e "$_m/aside-$_a" ]; do _a=$((_a + 1)); done
    mkdir "$_m/aside-$_a" || { [ -n "$MENU_DATA_MNT" ] || umount "$_m"; return 1; }
    for _f in "$_m"/* "$_m"/.[!.]*; do
        [ -e "$_f" ] || [ -L "$_f" ] || continue
        case "${_f##*/}" in aside-*) ;; *) mv "$_f" "$_m/aside-$_a/" ;; esac
    done
    sync
    [ -n "$MENU_DATA_MNT" ] || umount "$_m"
    DATA_ASIDE=aside-$_a
}
