# The shell and the simulator

`t48-shell` owns the screen and every input device, shows the idle screen and the launcher, and runs one app at a time over the [app protocol](app-protocol.md). The same program, started as `t48-sim`, is the PC simulator. Source in [`shell/`](../shell), library in [`libt48/`](../libt48). Design: [architecture.md](architecture.md#the-shell).

![idle](img/shell-idle.png)
![launcher](img/shell-launcher-open.png)
![the clock example app](img/app-clock.png)

## What this first version does

- Idle screen: time, date, network address, and a placeholder where the SIP account status will go.
- Launcher: a tile for each installed app (see [the manifest](app-protocol.md#the-manifest)). Arrows and OK, or touch. X goes back to idle.
- In an app: a short press on X goes to the app as "back"; holding X for 0.8 s is Home. The app is paused and frozen, and its tile says "open"; picking it again brings it back where it was. Picking a different app closes it.
- Lifting the handset takes the screen at once, from anything, and shows a placeholder call screen. Putting it down returns to what was there.
- An app that crashes, exits with an error, never starts or stops reading its events is cleaned up and the owner is told. Phone keys are never passed to apps.
- Apps run as their own unprivileged user when the shell runs as root (user number 20000 to 29999, worked out from the app id), with their own data folder under `/data/appdata`.

Not yet: dialler and real call screens (#26), settings screens (#23), installing apps (#22), backlight (#18), LEDs.

## Build

```
make -C shell                 # this PC: shell/build/t48-shell and t48-sim
make -C shell check           # run the simulator tests; pictures in shell/build/shots/
make -C shell CC=$HOME/src-build/t48s/bin/armcc O=build-arm all build-arm/apps    # the phone: static ARM
```

Plain make and a C compiler; nothing else. The fonts are committed as C (`make -C libt48 fonts` regenerates them).

## The simulator

```
t48-sim --apps DIR --script FILE        # run a script, exit 0 if every check passed
t48-sim --apps DIR --app clock --shot clock.png    # start one app, save its first picture
```

The screen is kept in memory; there is no window yet. Apps are the real programs talking the real protocol. A script is one command per line (`#` starts a comment):

| Command | |
|---|---|
| `key NAME [MS]` | press and release (held 60 ms, or MS). Names: `0`-`9`, `star`, `hash`, `up`, `down`, `left`, `right`, `ok`, `x`, `volup`, `voldown`, `headset`, `mute`, `message`, `hold`, `redial`, `transfer`, `speaker` |
| `keydown NAME`, `keyup NAME` | one half of a press |
| `touch X Y` | a tap; `touchdown X Y`, `touchmove X Y`, `touchup X Y` for a drag |
| `hook off`, `hook on` | lift and replace the handset |
| `wait MS` | |
| `waitframe [MS]` | until the app has drawn since the last input (fails after MS, default 3000) |
| `waitscreen NAME [MS]` | until the shell shows `idle`, `launcher`, `app`, `call` or `message` |
| `expect NAME` | fail unless that screen is showing now |
| `shot FILE` | save the screen as `.png` or `.ppm` |
| `quit` | |

`shell/tests/` has two scripts, run by `make check`: a tour, and apps that crash and hang.

## Running it on the phone

The kexec image starts `t48-shell` on the screen at boot (`kernel/kexec-rootfs/etc/inittab`), with the clock app in `/usr/share/t48/apps`. Its log is `/var/log/t48-shell.log` on the phone: the input devices it found, each screen change, each app start and stop. If it is killed, `init` starts it again.

![the idle screen, read back from the phone's framebuffer](img/shell-on-phone.png)

To put a new build into the image, copy the programs into the kernel build folder before building (the paths are the ones `kernel/kexec-rootfs.list` names):

```
make -C shell CC=$HOME/src-build/t48s/bin/armcc O=build-arm all build-arm/apps
cp shell/build-arm/t48-shell                 BUILD/kexec-rootfs/usr/bin/
cp -r shell/build-arm/apps/clock             BUILD/kexec-rootfs/usr/share/t48/apps/
```

To try a build without a new image, copy it over and restart the shell; extra apps can go in `/data/apps` (in RAM for now, so gone at the next boot):

```
tools/ourkernel.sh ssh 'cat > /usr/bin/t48-shell.new && chmod +x /usr/bin/t48-shell.new && mv /usr/bin/t48-shell.new /usr/bin/t48-shell && killall t48-shell' < shell/build-arm/t48-shell
tar -C shell/build-arm/apps -cf - crasher | tools/ourkernel.sh ssh 'tar -C /data/apps -xf -'
```

### Testing without being there

`tools/phone-input.sh` presses keys, touches the screen and lifts the handset from a PC, and saves the phone's real screen as a PNG. It uses test hooks in our keypad and touch drivers (`kernel/README.md`), so the events travel through the real input devices and the shell cannot tell them from a finger. The names are the simulator's:

```
tools/phone-input.sh key ok -- shot launcher.png
tools/phone-input.sh key x 1000          # hold X for a second: Home
tools/phone-input.sh touch 400 420       # screen pixels
tools/phone-input.sh hook off -- shot call.png -- hook on
tools/phone-input.sh log
```

### What has been checked on the phone (2026-11-23)

All with injected input, nobody at the phone:

- The idle screen draws at boot, and the clock on it follows NTP.
- OK, or a touch on Apps, opens the launcher; OK or a touch on its tile starts the clock, as its own user (uid 23373), with its own folder in `/data/appdata`.
- The clock shows key names and puts a dot under a touch. A touch asked for at 600, 60 arrives as 599, 59, so the calibration is applied; with a changed `/etc/pointercal` the same touch lands elsewhere, so the file is read.
- Holding X goes Home, the app is frozen (process state T), its tile says "open", and picking it resumes it. A short X closes it. Volume and the other phone keys are logged and do not reach the app.
- Hook off shows the call screen and freezes the app; hook on brings it back. A shell started with the handset already lifted goes straight to the call screen.
- The devices are grabbed: a second program reading `/dev/input/event0` sees nothing while keys are pressed, and nothing is typed on the text console underneath.
- `kill -9` on the app gives "Clock has stopped", no process or buffer left behind. The Crasher test app's three cases (crash, error exit, hang) all end the same way as in the simulator. Killing the shell takes its app with it, and `init` restarts the shell.

### Still needs fingers

1. Press each key on the idle screen, in the launcher and in the clock: digits, `*`, `#`, arrows, OK, X. The clock should name each one.
2. Hold X in the clock for a second: Home. Tap its tile: back in the clock.
3. Tap Apps, tap the clock, drag a finger around: the dot should follow under the finger, to the edges.
4. Lift the handset in the clock: the call screen at once. Put it down: the clock again.
5. Press Volume, Mute, Headset, Speakerphone, Hold, Transfer, Redial, Message in the clock: nothing happens on the screen, and `tools/phone-input.sh log` has a "phone key" line for each.
