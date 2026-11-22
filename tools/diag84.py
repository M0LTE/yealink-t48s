import sys,time; sys.path.insert(0,"tools")
from ylweb import Phone
from phonecfg import HOST, USER, PASSWORD
p=Phone(HOST,USER,PASSWORD); assert p.login()[0]
Q="m=mod_data&p=network-diagnosis&q=diagnose"
def run(target, typ="ping", wait=6):
    d={"type":typ,"TargetIP":target}
    out=p.post(Q,{"action":"start",**d}).text
    for _ in range(wait):
        time.sleep(1)
        o=p.post(Q,{"action":"getinfo",**d}).text
        if o.strip(): out=o
        if "statistics ---" in out or "bad address" in out: break
    p.post(Q,{"action":"stop",**d})
    return out
for t in sys.argv[1:]:
    print("##### TargetIP =",repr(t)); print(run(t)[:900]); print()
