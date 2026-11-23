# SPDX-License-Identifier: AGPL-3.0-or-later
"""mkwebfont.py IN.ttf OUT.woff2 [AXIS=VALUE...]: the ASCII part of a font as WOFF2, for
the web setup page (webconfig/fonts). The output is committed; this needs fonttools and
brotli, which building does not. With axes, a variable font is fixed at those values."""
import sys
from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

font = TTFont(sys.argv[1])
axes = {a.split("=")[0]: float(a.split("=")[1]) for a in sys.argv[3:]}
if axes:
    font = instancer.instantiateVariableFont(font, axes)
opt = subset.Options()
opt.flavor = "woff2"
opt.layout_features = ["kern", "liga"]
opt.name_IDs = [1, 2]
opt.notdef_outline = True
opt.hinting = False
s = subset.Subsetter(opt)
s.populate(unicodes=list(range(0x20, 0x7F)))
s.subset(font)
font.flavor = "woff2"
font.save(sys.argv[2])
