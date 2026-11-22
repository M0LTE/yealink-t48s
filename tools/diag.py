# SPDX-License-Identifier: AGPL-3.0-or-later
import sys,time; sys.path.insert(0,"tools")
from ylweb import Phone
from phonecfg import HOST, USER, PASSWORD
p=Phone(HOST,USER,PASSWORD); assert p.login()[0]
def run(target, typ="ping", wait=6):
    q="m=mod_data&p=network-diagnosis&q=diagnose"
    r=p.post(q+"&action=start",{"type":typ,"TargetIP":target})
    out=r.text
    for _ in range(wait):
        time.sleep(1)
        o=p.post(q+"&action=getinfo",{"type":typ}).text
        if o: out=o
        if "statistics ---" in out or "bad address" in out: break
    p.post(q+"&action=stop",{"type":typ})
    return out
for t in sys.argv[1:]:
    print("##### TargetIP =",repr(t)); print(run(t)[:800]); print()
