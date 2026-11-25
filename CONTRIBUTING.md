# Contributing

Pull requests are welcome, small or large, written by hand or with an AI tool. This page is the rules; the detail is in the docs it links. Apps have their own repository, [M0LTE/yealink-t48s-apps](https://github.com/M0LTE/yealink-t48s-apps), with a shorter page that points back here.

## Before you start

- **Small fixes:** open a pull request.
- **Anything bigger** (a new service, a change to the app protocol, the settings files, the flash layout, the install or update path, a new kernel driver): open an issue first and say what you plan. The design and its reasons are in [docs/architecture.md](docs/architecture.md); [docs/developing.md](docs/developing.md) says where each part lives.
- One change per pull request. Name the issue it closes.

## Licence

- AGPL-3.0-or-later ([LICENSE](LICENSE)), except code built into or loaded into a Linux kernel (`kernel/drivers/`, `kernel6/drivers/`, `kernel6/dts/`, `kernel6/patches/`, `kmod/`), which is GPL-2.0. By sending a pull request you license it under these terms.
- Every new source file starts with an `SPDX-License-Identifier` line. A patch carries the licence of the code it changes.
- No code under a licence that cannot be combined with these. Third-party code says where it came from, keeps its own licence file, and gets a row in [NOTICE.md](NOTICE.md).
- **Never Yealink's files.** No firmware, no code or pages from their firmware or web UI, no flash dumps, no keys or certificates, no backup of a phone. `.gitignore` keeps the usual folders out; check `git status` anyway. A backup holds the phone's private keys: it is secret.

## What a pull request needs

1. **The tests pass**, and you say which you ran. CI runs on `main`, not on pull requests, so this is on you.
2. **New behaviour has a test.** Screens: a script in `shell/tests/`. Web setup: `webconfig/tests/`. Calls: `shell/tests/phone/`. Scripts in `tools/`: a `tools/test-*.sh`.
3. **Pictures for anything on a screen**, before and after, from the simulator (below). For the T46S's small layout too if the change touches it.
4. **Docs updated** in the same pull request: the page in `docs/` for that part, and [docs/status.md](docs/status.md) if what works has changed.
5. **Tried on a phone** if it touches hardware, boot, flash or install: say what you ran and on which model. If you have no phone, say so; someone with one will try it.

## Testing without a phone

Plain `make`, a C compiler and python3 are enough for most of it.

```
make -C shell check              # the screens in the simulator; pictures in shell/build/shots/
make -C webconfig check          # the web setup page, then with the shell
sh tools/test-update.sh          # the updater, on files
sh tools/test-usbmount.sh        # USB sticks, on folders
tools/test-userenv.sh; tools/test-bootmode.sh; tools/test-live.sh
tools/test-owner.sh bytes SET BACKUP     # the on-phone installer on a flash in a file
```

The first four are what CI runs. Real calls (baresip, the audio service, a Kamailio on loopback) run in the `sip/Dockerfile` container: `sip/test/phone-test.sh`, and `ARM=1` for the ARM builds under qemu-arm ([docs/phone.md](docs/phone.md#testing)).

**The simulator** is the shell itself with the screen in memory. `t48-sim --apps DIR --script FILE` runs a script of keys, touches and checks; `shot FILE` in a script saves a picture; `--model t46s` is the small screen. Commands: [docs/shell.md](docs/shell.md#the-simulator). Pictures of all three themes: `docs/design/render.sh` and `docs/design/web.sh` ([docs/design/README.md](docs/design/README.md)).

**A whole build** is in [docs/developing.md](docs/developing.md#build); CI ([.github/workflows/build.yml](.github/workflows/build.yml)) is the exact recipe. A run started by hand in your fork builds the stick and the SDK.

## Trying a build on a phone

- **A 5 V supply of 1.5 A or more.** Yealink's 0.6 A adapter freezes the phone and USB sticks fail.
- **Start from a USB stick first.** Hold Speakerphone at power-on, press 2: the build runs from RAM and nothing is written. A normal power-up brings back what was there ([docs/live-image.md](docs/live-image.md)).
- **Never write the flash without a backup** of that phone, made by the installer or `tools/nandbackup.sh` and checked ([docs/backup.md](docs/backup.md)).
- **Never write** the loader, U-Boot, its environment, the logo or the safe-mode kernel and root filesystem. Recovery mode depends on them, and code that writes them will not be merged ([never brick](docs/architecture.md#never-brick)).
- The serial console is 3.3 V ([docs/status.md](docs/status.md#serial-console)).

## Code

- C, `-std=gnu99`, builds clean with `-Wall -Wextra`. The function's opening brace on its own line, other braces on the same line; small static functions, `snake_case`.
- Indent as the folder does: four spaces in `shell/`, `libt48/` and `webconfig/`; tabs in `audiod/`; Linux kernel style in kernel code.
- Each file opens with a comment that says what it is for and names its issue and doc. Comments say why, in plain sentences.
- Static and small: programs on the phone are statically linked against musl, and the kernel slot is 2.5 MB. Say what a new dependency costs in size.
- Shell scripts are POSIX `sh` for busybox.

## Words on the screen

Everything a person reads (labels, buttons, notices, errors, the web setup page, log lines) follows [docs/style.md](docs/style.md). In short:

- Say it once. State, not story: `Registered`, `No network`.
- The reader is competent: no definitions, no reassurance, no "you".
- No exclamation marks, no emoji, no "please", "simply", "just", "oops", "let's".
- One word for one thing: the table in style.md.
- Sentence case. Printed messages are one line: what is wrong, and the fix if it is not obvious.

The look (themes, grid, type) is [docs/design/README.md](docs/design/README.md). Screens take colours from the theme in use, never a fixed one.

## Writing

- **No em or en dashes**, anywhere: code, comments, docs, commit messages. Use a comma, a colon, a semicolon or a new sentence.
- **ASCII in anything printed**: the screen, logs, program output. `->` not an arrow.
- **Markdown is not hard-wrapped.** One paragraph is one line.
- Docs are short and plain, and link rather than repeat.

## Commit messages

```
Area: what changed, in the present tense

Why, and anything a reviewer would not see in the diff. One paragraph per line. Issue #N.

Co-Authored-By: Tool and model <address>
```

The area is the part changed, as in `git log`: `Shell`, `SIP`, `Docs`, `kernel6`, `Installer`, `Web setup`, a model (`T46S`). No full stop on the subject. A body is optional when the subject says it all.

## AI-assisted contributions

Welcome, and much of this repository was written that way. Two conditions:

- **You have read, checked and tested what you send.** The tests above pass on your machine, the pictures are from your run, and you can answer questions about the change. Untested output will be closed.
- **Say which tool.** A trailer on each commit naming the tool and model, as in this repository's history: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`, or the equivalent for yours.

Point your tool at this file, [docs/style.md](docs/style.md) and the docs for the part you are changing before it writes anything.
