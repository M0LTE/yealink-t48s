"""actionurls.py [show|set]: read or set the phone's Action URLs (66.82 web UI).
set: calls take the screen back from t48ui; boot completion pings t48ui's /status."""
import re, sys
sys.path.insert(0, __file__.rsplit("/", 1)[0])
from ylweb82 import OldPhone
from phonecfg import HOST, USER, PASSWORD

HIDE = "http://127.0.0.1:8123/hide"
WANT = {"AUIncomingCall": HIDE, "AUOffHook": HIDE, "AUCallOut": HIDE, "AUIdleToBusy": HIDE,
        "AUSetupCompleted": "http://127.0.0.1:8123/status"}

p = OldPhone("http://" + HOST, USER, PASSWORD)
assert p.login()[0], "login failed"
page = p.get("p=features-actionurl&q=load").text
fields = dict(re.findall(r'name="(AU[A-Za-z_]+)" value="([^"]*)"', page))
if len(sys.argv) > 1 and sys.argv[1] == "set":
    form = dict(fields)
    form.update(WANT)
    r = p.post("p=features-actionurl&q=write", form)
    print("write:", r.status_code)
    page = p.get("p=features-actionurl&q=load").text
    fields = dict(re.findall(r'name="(AU[A-Za-z_]+)" value="([^"]*)"', page))
for k, v in sorted(fields.items()):
    if v:
        print(f"{k} = {v}")
print(f"({sum(1 for v in fields.values() if v)} of {len(fields)} set)")
