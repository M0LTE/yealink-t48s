import sys,time; sys.path.insert(0,"tools")
from ylweb82 import OldPhone
from phonecfg import HOST, USER, PASSWORD
p=OldPhone("http://"+HOST,USER,PASSWORD); assert p.login()[0]
def run(ip, typ="ping", wait=8):
    r=p.post("p=network-diagnosis&q=command",{"type":typ,"ip":ip}).text.strip()
    out=[f"[start -> {r[:80]}]"]
    if r=="ok":
        for _ in range(wait):
            time.sleep(1)
            d=p.post("p=network-diagnosis&q=result&type="+typ,{}).text
            if d=="stop": break
            if d.strip(): out.append(d)
    p.get("p=network-diagnosis&q=stop&type="+typ)
    return "".join(out)
for t in sys.argv[1:]:
    print("##### ip =",repr(t)); print(run(t)[:1200]); print()
