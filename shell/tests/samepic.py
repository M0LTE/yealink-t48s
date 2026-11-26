# SPDX-License-Identifier: AGPL-3.0-or-later
"""samepic.py A.ppm B.ppm SKIP: exit 0 if the two pictures are the same below the first SKIP rows
(the header, whose clock may have moved on between them). For the dialler's part redraws."""
import sys


def rows(path):
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)          # P6, size, depth, pixels
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


wa, ha, a = rows(sys.argv[1])
wb, hb, b = rows(sys.argv[2])
skip = int(sys.argv[3]) * wa * 3
if (wa, ha) != (wb, hb) or a[skip:] != b[skip:]:
    bad = next(i for i in range(skip, len(a)) if a[i] != b[i]) // 3 if len(a) == len(b) else 0
    print("pictures differ, first at %d, %d" % (bad % wa, bad // wa))
    sys.exit(1)
