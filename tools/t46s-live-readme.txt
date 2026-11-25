Open firmware for the Yealink SIP-T46S: try-only set, @VERSION@
================================================================

Built from commit @COMMIT@
of https://github.com/M0LTE/yealink-t48s

UNTESTED. Nobody has started this on a T46S. It was worked out from
code the T46S shares with the T48S, which is the phone the firmware
runs on. Your start would be the first. Issue 10 on the project page
is where to report it.

It runs from memory. This kernel has no flash driver, so nothing of
ours can be written to the phone. Unplugging the power ends it.

For the T46S only. Do not use the T48S stick or its Install page on
a T46S.

You need
--------
- A USB 2.0 stick, 1 to 32 GB: one partition, MBR partition table,
  FAT32.
- A 5 V power supply of 1.5 A or more. On a smaller one the phone
  can freeze while it reads the stick.
- A camera.

First start
-----------
1. Photograph the label under the phone, and Yealink's Status page
   (firmware and hardware version).
2. Copy T46S.rom, T46S.bin and T46S.rfs to the top folder of the
   stick. Eject it properly.
3. Unplug the phone's power (or its network cable, if that powers
   it). Put the stick in the phone's USB socket.
4. Hold SPEAKERPHONE and plug the power in. Keep holding until the
   screen shows a menu, about 5 seconds:
       1. TFTP    2. USB
   Let go. Do NOT press OK at this menu: OK resets the phone's
   settings. Photograph the menu.
5. Press 2. Within a minute the screen should show scrolling text,
   then a clock. Photograph each stage, whatever it shows.

To go back to Yealink's firmware
--------------------------------
1. Unplug the power.
2. TAKE THE STICK OUT. With the stick left in, Yealink's firmware
   tries to update itself from it and resets its settings.
3. Plug the power in and hold nothing. If it sits at the logo for
   more than two minutes, unplug and plug in once more.

What to report, on issue 10
---------------------------
- The photos: label, Status page, the menu, every stage after 2.
- Is the picture the right way up? Top left should be a green
  square, top right a red one. Blue instead of red: say so.
- Which keys work: digits, arrows, OK, X, the four soft keys under
  the screen, volume, handset on and off the hook.
- If a key does the wrong thing or nothing: hold X for 5 seconds
  at the clock. A key test opens. Press every key and
  photograph the screen after each. Hold any key for 3 seconds to
  leave.
- Settings, Screen: does the brightness change?
- The exact text of any message, and where it stopped.

The full list of checks is "Test plan for a T46S owner" in
docs/models.md on the project page. The line keys and the lamps in
the keys do nothing in this set.

If it does not start
--------------------
- No menu: Speakerphone was let go too early. Unplug, try again.
- Stops at "Start Updating...": most often the power supply.
- "Usb failed": the stick was not read. Check its format, or try
  another plain USB 2.0 stick.
- "Rom ... Err" or "Update failed": photograph it. The phone is
  unchanged.
- Blank or frozen after "Start Updating...": our kernel did not
  come up. Photograph the last thing on the screen.
In every case: unplug, take the stick out, power up normally.

Over the network instead
------------------------
Menu option 1 takes the same three files from a TFTP server.
docs/models.md has the steps. After a start that way Yealink's
firmware resets its own settings, so the stick is the gentler route.

Files
-----
T46S.rom  not firmware: a 4 KB label the phone checks before it
          starts the other two
T46S.bin  the kernel, Linux 6.18, built for the T46S
T46S.rfs  the system image, the same one the T48S runs
SHA256SUMS

Not affiliated with or endorsed by Yealink. Yealink is a trademark
of its owner. Use at your own risk.

Licences: the kernel is GPL-2.0, the rest of the firmware is
AGPL-3.0-or-later, with other free software under its own licences.
NOTICE.md on the project page lists them and says where the source
is.
