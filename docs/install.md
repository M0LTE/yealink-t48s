# Installing to flash

Issues [#8](https://github.com/M0LTE/yealink-t48s/issues/8) (installer), [#29](https://github.com/M0LTE/yealink-t48s/issues/29) (layout), [#30](https://github.com/M0LTE/yealink-t48s/issues/30) (way back), [#31](https://github.com/M0LTE/yealink-t48s/issues/31) (updates), [#32](https://github.com/M0LTE/yealink-t48s/issues/32) (rescue).

**Status, 2026-11-24: installed on Tom's phone.** First from a PC with `tools/t48install.sh` (below), then restored and installed again from the phone's own screen ([installer.md](installer.md)), which is the owner's route ([guide.md](guide.md)). Every later change reached the phone through its own updater.

After the install the phone starts our firmware by itself at power-on. Yealink's firmware is gone from the phone until it is restored from the backup.

## The install from a PC

Written as the plan before the first install, and carried out as written.

**What is written**

| Partition | Size | Before | After |
|---|---|---|---|
| `kernel` | 2.5 MB | Yealink's kernel | our kernel with its first stage (2.2 MB) |
| `app` | 75 MB | Yealink's firmware, and our `/data` in its last 60 blocks | ours: system slot A, system slot B, `/data` |

Nothing else. The loader, U-Boot, both of its settings partitions, the logo, the safe-mode kernel and root filesystem, and Yealink's `rfs`, `cfg` and `data` are never written, and the driver cannot write them.

**In what order, and how long.** One command on the PC, about 8 minutes, the phone stays on its normal screen throughout:

1. Checks, 2 minutes, nothing written: the backup against its checksums and the ones in [backup.md](backup.md); the phone's flash against the backup; the new kernel file as U-Boot will read it.
2. The settings in `/data` are copied to the PC.
3. `kernel` is erased and written (10 seconds), then read back to the PC and compared.
4. `app`: Yealink's 539 blocks are erased; the 60 blocks holding `/data` are not touched. The system image goes into slot A and is read back. `/data` is copied into a larger volume, compared file by file, and only then is the old one removed (3 minutes).
5. The whole of `app` is read back to the PC and checked, and every partition we never write is compared with how it was before (2 minutes).
6. It says `INSTALLED`. The phone is restarted by hand or with `reboot`.

**What you see.** After the restart: the Yealink logo from U-Boot as always, then our clock screen about 20 seconds later, registered, with the same account, keys and apps as before. From step 4 until that restart `/data` is a copy in RAM, so settings changed during the install are lost.

**What changes for development.** There is no Yealink firmware left to start our kernel from, so `tools/ourkernel.sh back` and `boot` stop working on this phone. A new system image goes in with `t48-update install` and falls back by itself if it is bad; a new kernel goes in with `t48-update kernel` or is tried first through recovery mode. The kernel written here carries our test builds' SSH keys, so rescue answers SSH without anyone reading a password off the screen.

**How we go back.** `tools/t48install.sh restore BACKUP`, about 8 minutes: it puts Yealink's `app` and `kernel` back from the backup, every page compared, and the phone is byte for byte as it was. If our firmware does not start at all, recovery mode still does (hold Speaker at power-on: it is in U-Boot, which is never written), and the restore runs from there.

**What could go wrong**

| | What happens | What to do |
|---|---|---|
| Power lost while `kernel` is written (a 10 second window) | U-Boot finds no kernel. The screen stays on the logo | Recovery mode over TFTP, then run install or restore again |
| Power lost while `app` is written | The phone starts our kernel, which finds no system and shows the rescue screen with SSH | Run install again from the PC. Nothing at the phone |
| A write error or a bad block appears | The installer stops and says what to run | Run it again; it carries on from where it is |
| Yealink's firmware meets U-Boot's leftover `BOOTMODE` flag after a restore | It resets its own settings at first start | Clear the flag first (the rescue TFTP set does it); the restore says so if the flag is set |

Not possible by construction: writing U-Boot, its settings, or the recovery partitions. The driver has a compiled-in list of two partitions and tests at every start that everything else is refused.

## Layout after the install

| Where | What |
|---|---|
| `kernel`, 0x015a0000, 20 blocks | a U-Boot image: Linux 6.18 with the device tree and the first stage inside. Read by U-Boot's own boot command, unchanged |
| `app`, 0x02720000, 599 blocks | UBI |
| volume `system-a`, `system-b` | 160 blocks (20 MB) each. A whole system image, read into RAM at start. Checksummed by UBI |
| volume `bootstate0`, `bootstate1` | one block each. Which slot is active, and whether one is on trial |
| volume `data` | about 210 blocks (26 MB, 22 MB usable; today 6 MB). `/data`, as today |
| unallocated | about 60 blocks, for UBI to move data into when a block wears out |

**How U-Boot starts it.** Its boot command is `nboot 0x02680000 0 0x015A0000`: it reads the image header at the start of `kernel`, takes the size from it, reads that much to RAM, checks two CRCs and starts it. That is the same check and the same hand-over as the recovery-mode start that passed on 2026-11-24, with the file coming from flash instead of TFTP. `tools/t48flash.py nboot` does U-Boot's read on a PC, and is checked against Yealink's own kernel partition from the backup.

**How `/data` moves.** UBI does not record where its blocks are, so the 60 blocks that hold `/data` today are simply found again when UBI is given the whole partition. No copy is needed for that. The installer then copies it once, inside UBI, into a volume with more room.

## Updates and fallback

Settings, Update on the phone ([settings.md](settings.md)) and the Update page of web setup ([web.md](web.md)) are front ends for one command:

```
t48-update status
t48-update channel [stable|testing|URL]     # where updates come from; stable unless changed
t48-update check                            # ask the channel; writes nothing
t48-update install                          # the channel's system image, into the slot not in use
t48-update kernel                           # the channel's kernel, only when asked
t48-update install FILE-or-URL [SHA256]     # by hand
t48-update kernel FILE-or-URL SHA256
t48-update switch                           # try the other slot once
t48-update daily [now|due]                  # the daily check (below)
```

Nothing is installed by itself: the phone installs when asked. It checks when asked, and once a day.

### The daily check

Issue [#80](https://github.com/M0LTE/yealink-t48s/issues/80). The runit service `update-check` runs `t48-update daily`, which asks the phone's channel for its newest release (as `check` does: stable the newest release, testing `system-latest`, a URL its `VERSION`) and the app feeds for newer versions of installed apps, each app from the feed it came from (`t48-app check`, [apps.md](apps.md#feeds)). It never installs.

- **When.** Once a night at the phone's own time, chosen at random between 01:00 and 05:00 in its time zone and kept in `/data/phone/update-check`, so phones spread out. And three minutes after a start when the last check is over a day old, which also covers a night the phone was off. Not before the clock has been set.
- **What it tells.** The home screen's notices (`Update available`, `2 app updates`: [settings.md](settings.md#update)) and the web setup page's overview. What it found is kept in `/data/phone/update-latest` with the version it was compared with, so it survives a restart and stops counting once that release runs.
- **Quiet.** It never touches the Update page's status: `Checking` is not shown, and `Restart to finish` or a failed install's words stay as they were. VERSION, SHA256SUMS each get a minute, each feed's index 15 seconds. **Failures** show nothing, and what the last check found stays. Each check is one line in `/var/log/t48/update-check/current`: `daily check: firmware 20261126-1a2b3c4 available, 2 app updates`, `..., apps up to date, 1 feed not reached` when one of several feeds was not, or `daily check: firmware not reached, apps not reached (cannot reach the server)`. The next try is the next night.
- **On a phone started from a USB stick** it asks the app feed only.
- **Off** under Settings, Update, Daily check, on the web setup page's Update page (both also on a phone that is not installed, and while an update runs), or `daily=off` in `/data/config/update`. The only traffic is two small files from the channel and the feed's index.

**Channels.** A channel is a folder on a web server with four files: `VERSION` (one line), `SHA256SUMS`, `system.img` and `T48S-kernel.uimg`.

| Channel | What | Where |
|---|---|---|
| `stable` | the newest `v*` release | `github.com/M0LTE/yealink-t48s/releases/latest/download` |
| `testing` | the newest build of `main`, the rolling pre-release `system-latest` | `.../releases/download/system-latest` |
| a URL | the owner's own builds | any `http://` or `https://` folder with the four files |

Downloads use HTTPS with the image's own certificate list, and each file is compared with its SHA-256 in `SHA256SUMS` before anything is written. Releases are not signed yet: the check proves the download is whole, and HTTPS that it came from GitHub.

**System image.** It is written to the other slot and read back. At the next start it gets one try, and the try is counted before it starts. If the phone is still up a minute later with the shell running, that slot becomes the active one. If not (it hangs and the watchdog resets it, it does not mount, or the phone is restarted within the minute) the next start goes back to the slot that worked. Nobody has to be at the phone. The result is kept and shown as the last result: `20261124-ac7a8f4 installed`, or `20261124-ac7a8f4 did not start, went back`.

**Kernel.** One slot, because U-Boot's boot command is fixed, so `install` never writes it. `check` compares the kernel partition with the channel's kernel and says when they differ; `t48-update kernel` (Install kernel on the screen) then writes it, with the driver's page-by-page comparison and a second read-back of the whole image. A bad kernel is recovered through recovery mode. The kernel holds the first stage, so the rescue menu changes with it. One of our own test builds, with SSH keys in its first stage, always differs from the published kernel.

Proved on Tom's phone on 2026-11-24, each step through the updater: see [the record](#self-update-on-the-phone-2026-11-24).

## Rescue

| Failure | Result |
|---|---|
| The slot on trial does not start | the active slot starts, by itself |
| The active slot is unreadable | the other slot starts |
| No slot starts, or the install was cut short | the rescue menu, then rescue by itself after 30 seconds: screen, DHCP, SSH, serial shell |
| The kernel hangs | U-Boot's watchdog resets the phone after 90 seconds, and again; recovery mode gets out of it |
| The kernel partition is unreadable | U-Boot stops at the logo; recovery mode |

**The rescue menu.** Hold X while the phone starts (from power-on until the menu shows, about 3 seconds after the logo). Up, Down and OK, or the number:

| | |
|---|---|
| Start | carry on as usual. Also what happens after a minute with no key |
| Start slot b | the other slot, for this start only. Nothing is written down, and it does not become the active slot |
| Start with settings wiped | asks first. Everything in `/data` (settings, account, keys, apps) is moved into `/data/aside-1`, then `aside-2` and so on; nothing is deleted. The phone starts as new. Moving the files back restores them |
| Rescue | the first stage stays up: address on the screen, SSH (a one-time password on the screen, or the built-in keys on one of our test builds), serial shell |
| Restore help | how to go back to Yealink's firmware |

When no slot starts, the menu has Rescue, Restart and Restore help.

Recovery mode and safe mode are Yealink's and are untouched.

## The lock

The driver ([kernel6/README.md](../kernel6/README.md#flash)) can write `kernel` and `app` and nothing else, and both start every boot locked. `app` opens by itself only when the kernel was started from flash on an installed phone. Otherwise the installer opens what it needs and closes `kernel` again when it has finished. At every start the driver tries the writes it must refuse (549 attempts and questions), with the lock closed, with it open, and around the one `userenv` block of [installer.md](installer.md#the-bootmode-flag-the-one-write-outside-kernel-and-app), and gives no flash at all if one gets through.

## What was shown before writing

| | How |
|---|---|
| The bytes of the kernel partition | worked out on the PC with error correction; the phone works out the same bytes independently (`dry-run`) |
| U-Boot will read it | U-Boot's read done on a PC over those bytes gives the kernel file back; the same code passes Yealink's own kernel from the backup |
| Nothing else can be written | the driver's self-test on the phone at every start; the same rule in the PC rehearsal |
| Install, update, failed update, damaged slot, writes cut short, `/data` kept | the phone's own scripts on Linux's simulated flash chip with real UBI (`tools/test-install.sh ubi`), 27 checks |
| The way back gives the stock flash | on a copy of the whole flash in a file: after install and restore it is identical to the backup (`tools/test-install.sh bytes`) |
| The phone as it is today | `tools/t48install.sh dry-run`, run 2026-11-24: backup matches its checksums and backup.md; ubl, uboot, env, userenv, logo, smkern, smrfs, rfs, kernel and app on the phone are identical to the backup; nothing written, lock never opened |

The write then showed the rest: U-Boot's normal boot command started our kernel from flash, and the first stage found slot a (2026-11-24, [picture](img/installed-first-boot.png)). Not tried: a write to `kernel` or `app` from the phone's command line with the lock closed. The driver's self-test makes that attempt itself at every start, at all three layers.

## Commands

```
tools/t48install.sh set SET kernel=T48S-kernel.uimg system=system.img tools=DIR
tools/t48install.sh check   SET BACKUP      # reads only
tools/t48install.sh dry-run SET BACKUP      # reads only
tools/t48install.sh install SET BACKUP      # asks for the word "install"
tools/t48install.sh restore BACKUP          # asks for the word "restore"
tools/test-install.sh bytes SET BACKUP      # no phone
sudo tools/test-install.sh ubi SET          # no phone
```

`T48S-kernel.uimg` is `kernel6/build.sh` with `FLASH=1`. A phone with a bad block in `kernel` or `app` is refused for now.

## The set of the first install

Built 2026-11-24, with our test builds' SSH keys in the kernel's first stage. On the development machine in `dumps/install/set/` (not in the repository). The dry run, both rehearsals above and the install were run with exactly these files.

```
bfe99abcb71c43b3ecaa6381d21f46b8946e0cda00b5639da53d75877cdcb7bb  T48S-kernel.uimg
369ad67af9ee316210cf8f4f615b776a939c9a3ce4def938875d6b0e4f66dd6d  system.img
dea5687047e26cb7348747a309299bda3b98db5dcf8ef45e375c9dde46d6ffab  kernel.dataoob
98f22d9558e51923963bd079112456f2bd09f71df7276045782788809d1b2d94  nandrestore
335207fc992419e162b225d2fa2a64c99a61e78642a3dbdaeb9d14fa2f7b8921  nandread
```

The backup is `dumps/stock-backup/`, with a second copy on studybox ([backup.md](backup.md)).

## Self-update on the phone, 2026-11-24

Every change after the install reached Tom's phone through `t48-update` on the phone itself. `/data` and the SIP registration were checked after each restart.

| | What | Result |
|---|---|---|
| 1 | The install's image (slot a) fetched `system.img` of the `system-latest` pre-release from GitHub over HTTPS, with its SHA-256 | written to slot b in 6 seconds; after the restart `20261124-1d14020` ran on trial and became the active slot a minute later |
| 2 | `t48-update channel testing`, `check` | `Up to date`, `Kernel differs` (the phone's kernel is a test build with keys). `stable` says `No release found` until there is a `v*` release with these files |
| 3 | On purpose: an image of random bytes behind a valid first four, from a PC on the network | accepted by its checksum, written to slot a. At the restart it did not mount; the first stage started slot b in the same start, by itself |
| 4 | A good image, `dev2-f595541`, from the PC | slot a, on trial, confirmed |
| 5 | On purpose: a real image whose start-up restarts the phone after 25 seconds | started in slot b on its one try, restarted itself, and the next start went back to slot a by itself. Last result: `ff503d9d2c25 did not start, went back` |
| 6 | Settings, Update on the phone's screen, Check now | `Latest 20261124-1d14020`, `Update available` ([picture](img/update-screen.png)) |
| 7 | After main was merged and CI had published again: `t48-update install` with no file, from the testing channel | `20261124-79cde57` downloaded, checked against `SHA256SUMS`, written to slot b, on trial, confirmed. That is what the phone runs now |

The serial console, dead after every start from U-Boot until then (issue 59), answered from step 1 on.

**The kernel** with the rescue menu and the serial fix (issues #32, #59) reached the phone the same day in the kernel partition, written by the on-phone installer from the release stick 20261124-56eedca. Its parts were tested before that (the menu on the phone's own screen with keys injected, [picture](img/rescue-menu.png); the first stage's nothing-starts path in QEMU). Holding X at a real power-on has not been recorded as tried.

