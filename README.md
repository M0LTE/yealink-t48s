# yealink-t48s

Open firmware for the Yealink SIP-T48S desk phone: a current Linux kernel, a SIP phone with six accounts, contacts and call history, apps, a web setup page, SSH. It replaces Yealink's firmware, and the phone's own screen puts Yealink's back.

![the idle screen](docs/design/switchboard/phone-idle.png)

## Start

1. A 5 V power supply of 1.5 A or more. Yealink's 0.6 A adapter is too small.
2. Unpack `t48s-stick.zip` from the [latest release](https://github.com/M0LTE/yealink-t48s/releases/latest) onto a FAT32 USB stick.
3. Stick in the phone. Hold **Speakerphone** while plugging the power in, then press **2**. The firmware runs from the stick; nothing is written.
4. To keep it: **Settings, Install**. It backs the phone up to the stick first.

Every step, the way back and what the screen messages mean: **[docs/guide.md](docs/guide.md)**.

Not yet proven on a phone other than the author's. Where things stand: [docs/status.md](docs/status.md).

## More

- Apps for the phone: [M0LTE/yealink-t48s-apps](https://github.com/M0LTE/yealink-t48s-apps), written against the app SDK ([sdk/README.md](sdk/README.md)).
- How it is built and how it works: [docs/developing.md](docs/developing.md).

![Waterfall of GB7RDG](docs/img/waterfall-gb7rdg.png)

## Licence

AGPL-3.0-or-later (see `LICENSE`), except code that is built into or loaded into a Linux kernel (`kernel/drivers/`, `kmod/`), which is GPL-2.0 because the kernel is GPLv2-only. Third-party programs we build or bundle (busybox, dropbear, musl, Linux itself) keep their own licences. Vendor firmware and dumps are not in the repository.
