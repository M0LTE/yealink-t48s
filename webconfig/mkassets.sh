#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# mkassets.sh style.css t48.js regular.woff2 bold.woff2 > assets.c: the page's
# style sheet and script as C strings and its two fonts as bytes, so the server
# is one file with nothing beside it to serve.
one() {
    printf 'const char %s[] =\n' "$1"
    sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/    "/' -e 's/$/\\n"/' "$2"
    printf '    ;\n'
}
bytes() {
    printf 'const unsigned char %s[] = {\n' "$1"
    od -An -v -tu1 "$2" | sed -e 's/^ *//' -e 's/  */,/g' -e 's/$/,/'
    printf '};\nconst unsigned %s_len = sizeof %s;\n' "$1" "$1"
}
printf '/* made by mkassets.sh from the style sheet, t48.js and the two fonts: change those */\n#include "web.h"\n'
one asset_css "$1"
one asset_js "$2"
bytes asset_font_regular "$3"
bytes asset_font_bold "$4"
