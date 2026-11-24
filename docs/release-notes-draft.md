<!-- Draft for the first release. Paste what is below this line into the release when tagging; CI's own text lists the files. -->

Open firmware for the Yealink SIP-T48S desk phone. First release.

## What it is

A replacement for Yealink's firmware: Linux 6.18, a SIP phone, apps, a web setup page. Try it from a USB stick without changing the phone. Install it from the phone's own screen, and go back to Yealink's firmware the same way.

## What works

- Calls on the handset and the speaker: hold, mute, transfer, a second call, do not disturb, voicemail.
- Six SIP accounts, contacts, the call list.
- Touchscreen, keys, lamps, backlight, Ethernet.
- Apps from a feed ([yealink-t48s-apps](https://github.com/M0LTE/yealink-t48s-apps)), and an SDK to write them.
- Three themes.
- Web setup from a browser on the same network, paired with a code the phone shows.
- Updates over the network, stable and testing channels. An update that does not start falls back by itself.
- SSH, off until turned on at the phone.
- A rescue menu, X held at power-on. New, and the least tried part.

## Known limits

- It has been installed on one phone, the author's. Yours would be the second. Install only a phone you could do without.
- T48S only. A T46S build exists to try over the network and nobody has started it.
- A phone with a bad flash block where the firmware goes cannot be installed yet. The installer says so and writes nothing.
- On the speaker, one side is heard at a time.
- The headset socket is untried.
- Releases are not signed.

## The power supply

Use a 5 V supply of 1.5 A or more. Yealink's own adapter is 0.6 A and the phone draws up to 0.9 A: on it the phone freezes and USB sticks are not read. This is the first thing to check when anything goes wrong.

## Install

1. Unpack `t48s-stick.zip` onto a FAT32 USB stick (MBR, one partition).
2. Stick in the phone. Hold **Speakerphone** while plugging the power in, until the menu shows. Press **2**. The firmware now runs from the stick; nothing is written.
3. **Settings, Install, Back up.** The phone's whole flash is copied to the stick and checked. About 3 minutes.
4. **Install.** About 3 minutes. Take the stick out when the screen says so; the phone restarts.
5. Copy the `t48s-backup` folder from the stick to a computer and keep it.

Every step: [docs/guide.md](https://github.com/M0LTE/yealink-t48s/blob/main/docs/guide.md).

## Go back

Start from the stick that holds `t48s-backup`, then **Settings, Install, Restore Yealink firmware**. About 3 minutes. The phone is then byte for byte as the backup found it.

## Notice

Not affiliated with or endorsed by Yealink. Yealink is a trademark of its owner. Use at your own risk: installing replaces the phone's firmware, and the backup is the way back.

Licences are in [NOTICE.md](https://github.com/M0LTE/yealink-t48s/blob/main/NOTICE.md). `t48s-sources.tar.gz` is the source for the third-party parts of these files (Linux, busybox and the rest).
