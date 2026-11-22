# SPDX-License-Identifier: AGPL-3.0-or-later
"""shot.py OUT.png [SOURCE]: grab the phone's screen (or SOURCE file, e.g. /tmp/fbswap.shadow) as a PNG, over SSH."""
import subprocess, sys
from PIL import Image
sys.path.insert(0, __file__.rsplit("/", 1)[0])
from phonecfg import HOST
src = sys.argv[2] if len(sys.argv) > 2 else "/dev/fb0"
raw = subprocess.run(["ssh", "-o", "BatchMode=yes", f"root@{HOST}", f"dd if={src} bs=3200 count=480 2>/dev/null"],
                     capture_output=True, timeout=60).stdout
Image.frombytes("RGB", (800, 480), raw, "raw", "BGRX").save(sys.argv[1])
print(sys.argv[1], len(raw))
