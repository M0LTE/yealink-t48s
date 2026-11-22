# SPDX-License-Identifier: AGPL-3.0-or-later
"""linekey.py [show | set N LABEL URL]: read the line keys on page 1, or make key N a URL key (66.82 web UI)."""
import re, sys
sys.path.insert(0, __file__.rsplit("/", 1)[0])
from ylweb82 import OldPhone
from phonecfg import HOST, USER, PASSWORD

Q = "p=dsskey&model=1&q=load&linepage=1"
p = OldPhone("http://" + HOST, USER, PASSWORD)
assert p.login()[0], "login failed"


def fields(html):
    """Form fields as a browser would submit them: enabled inputs and selected options."""
    out = {}
    for m in re.finditer(r'<input([^>]*)>', html):
        a = m.group(1)
        n = re.search(r'name="([^"]+)"', a)
        if not n or 'disabled' in a or re.search(r'type="(button|submit)"', a):
            continue
        v = re.search(r'value="([^"]*)"', a)
        out[n.group(1)] = v.group(1) if v else ""
    # the page sets each dropdown with SetSelectValue(document.getElementById("ID"), "V") after it
    setvals = dict(re.findall(r'SetSelectValue\(document\.getElementById\("([^"]+)"\),\s*"([^"]*)"\)', html))
    for m in re.finditer(r'<select([^>]*)>(.*?)</select>', html, re.S):
        n = re.search(r'name="([^"]+)"', m.group(1))
        i = re.search(r'id="([^"]+)"', m.group(1))
        if not n or 'disabled' in m.group(1):
            continue
        if i and i.group(1) in setvals:
            out[n.group(1)] = setvals[i.group(1)]
            continue
        sel = re.search(r'<option[^>]*value="([^"]*)"[^>]*selected', m.group(2)) or \
              re.search(r'<option[^>]*value="([^"]*)"', m.group(2))
        out[n.group(1)] = sel.group(1) if sel else ""
    return out


def show(f):
    for i in range(1, 12):
        if f"selType_{i}" in f:
            print(f"key {i:2}: type {f.get(f'selType_{i}'):>3}  line {f.get(f'selLine_{i}','')!s:2} label {f.get(f'memLabel_{i}','')!r:12} value {f.get(f'memValue_{i}','')!r}")


html = p.get(Q).text
f = fields(html)
if len(sys.argv) == 5 and sys.argv[1] == "set":
    n, label, url = sys.argv[2], sys.argv[3], sys.argv[4]
    f.update({f"selType_{n}": "17", f"memLabel_{n}": label, f"memValue_{n}": url})
    f.pop("LanguageWebLang", None)
    r = p.post("p=dsskey&q=write&model=1&linepage=1", f)
    print("write:", r.status_code)
    f = fields(p.get(Q).text)
show(f)
