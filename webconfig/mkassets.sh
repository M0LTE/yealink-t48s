#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# mkassets.sh style.css t48.js > assets.c: the page's style sheet and script as
# C strings, so the server is one file with nothing beside it to serve.
one() {
    printf 'const char %s[] =\n' "$1"
    sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/    "/' -e 's/$/\\n"/' "$2"
    printf '    ;\n'
}
printf '/* made by mkassets.sh from style.css and t48.js: change those */\n#include "web.h"\n'
one asset_css "$1"
one asset_js "$2"
