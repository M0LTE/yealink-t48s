#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# The simulator's stand-in for t48-apply (make check gives it as --apply). The
# access commands run the phone's real t48-access script, with its files under
# simdata/access and with stubs/ in place of dropbear, dropbearkey and mkpasswd,
# so nothing on this PC is touched. A restart is only noted in a file.
# Everything else t48-apply does is left out.
HERE=$(cd "$(dirname "$0")" && pwd)
case "$1" in
access) set -- apply ;;
access-*) set -- "${1#access-}" ;;
restart) mkdir -p simdata && date +%s >> simdata/restart.asked; exit 0 ;;
*) exit 0 ;;
esac
SIM=$PWD/simdata
mkdir -p "$SIM/access/etc"
[ -f "$SIM/access/etc/shadow" ] || echo 'root:*:::::::' > "$SIM/access/etc/shadow"
export T48_ACCESS_KEEP="$SIM/access/keep" T48_ACCESS_RUN="$SIM/access" T48_ACCESS_DEV="$SIM/access/dev"
export T48_ACCESS_SHADOW="$SIM/access/etc/shadow" T48_ACCESS_DOOR_SECONDS=${T48_ACCESS_DOOR_SECONDS:-20}
export SIM_BIN=$PWD PATH="$HERE/stubs:$PATH"
exec sh "$HERE/../../../buildroot/board/t48s/rootfs-overlay/usr/bin/t48-access" "$@"
