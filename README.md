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
  - `shot.py` - screenshot of the phone (or of Yealink's hidden screen) as PNG
  - `actionurls.py`, `linekey.py` - read or set Yealink's Action URLs and line keys
  - `mkfont.py` - generates the fonts for `membanner`
  - `phonecfg.py` - phone address; reads the web login from `~/.config/yealink-t48s/web-login` (`user:password`) or `T48S_LOGIN`
- `native/` - programs for the phone, built with clang (`make`)
- `docs/` - notes
- `shots/`, `frames/` - screenshots and test frames

## Logging in

SSH starts at boot: `ssh root@<phone>` (key login; keys in `/data/ssh/authorized_keys`). Copy files with `scp -O`.

If SSH is ever gone (for example after a factory reset), the web trick still gives root:

```sh
cd tools
python3 diag82.py '127.0.0.1 -c 1;/usr/sbin/telnetd -p 2323 -l /bin/sh'
python3 tsh.py 'id'
```

Never upgrade the phone's firmware: 66.84 and later close the hole this relies on.

## Apps

The pdn-soundmodem receive waterfall and the GB7RDG links screen that used to live in `native/` are apps now, in [M0LTE/yealink-t48s-apps](https://github.com/M0LTE/yealink-t48s-apps), built against the app SDK ([sdk/README.md](sdk/README.md)) and run by the shell ([docs/shell.md](docs/shell.md)).

![Waterfall of GB7RDG](docs/img/waterfall-gb7rdg.png)

## Licence

AGPL-3.0-or-later (see `LICENSE`), except code that is built into or loaded into a Linux kernel (`kernel/drivers/`, `kmod/`), which is GPL-2.0 because the kernel is GPLv2-only. Third-party programs we build or bundle (busybox, dropbear, musl, Linux itself) keep their own licences. Vendor firmware and dumps are not in the repository.
