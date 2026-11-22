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
  - `mkfont.py` - generates the fonts for `t48ui` and `waterfall`
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

## Waterfall

`native/waterfall` shows a pdn-soundmodem station's receive waterfall full screen: frequency scale, live spectrum, the modem bands, decoded frames tagged on their bursts with callsign and SNR, and a list of recent frames. It only listens to the station page (it answers the page's keep-alive and nothing else), and reconnects by itself if the station goes away.

![Waterfall of GB7RDG](docs/img/waterfall-gb7rdg.png)

```sh
cd native && make waterfall           # static ARM binary (musl)
scp -O waterfall root@<phone>:/tmp/
ssh root@<phone> /tmp/waterfall -n GB7RDG ws://10.45.0.37:8099/
```

It draws on `/dev/fb0` (800x480, 32 bpp) and switches the text console off while it runs; Ctrl-C or `kill` gives the console back. `-n` sets the name in the header (default: the host). To try it on a PC, `make waterfall-host` and `./waterfall-host -n GB7RDG --shot out.ppm --seconds 60 ws://10.45.0.37:8099/` renders 60 seconds into an image file instead.
