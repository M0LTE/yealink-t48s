# Synthetic SIP service for testing

A small SIP server on the LAN so the phone has something real to register to and call. Set up 2026-11-23.

- **Where:** Proxmox container 156 `sip` on proxmox1, Debian 13, DHCP address 10.45.0.147 (no reservation yet), starts on boot.
- **Registrar and proxy:** Kamailio 6.0 (`/etc/kamailio/kamailio.cfg`), UDP and TCP on port 5060, domain `10.45.0.147`. Digest authentication on REGISTER and INVITE; accounts live in an `htable` filled at start-up from the config file. Calls between registered users are proxied (record-routed); media goes directly between the phones.
- **Accounts:** `t48s` (the phone), `tom` (for a softphone), `echo` (the echo bot). Passwords are random and kept only in `~/.config/yealink-t48s/sip-account` on the claude-code container and in the server's config (mode 640), never in this repository.
- **Echo bot:** `echobot.service` runs baresip as an unprivileged user, registered as `echo`, auto-answering and looping received audio straight back (`aubridge` as both player and source). Call `echo`, or `600` from a keypad, to hear yourself. Tested 2026-11-23: a 700 Hz tone sent from a second client came back at the same frequency and full level (G.711).

To give the phone its account, write `/data/config/sip` on it (see [settings.md](settings.md)) with `user=t48s`, `domain=10.45.0.147` and the password, or enter it on the phone under Settings > SIP account.
