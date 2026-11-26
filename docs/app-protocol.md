# The app protocol

API level 6. This is the contract between the shell (`t48-shell`) and an app. `libt48` wraps it for C programs, but an app in any language can speak it directly. The C definitions are in [`libt48/include/t48_proto.h`](../libt48/include/t48_proto.h). The design reasons are in [architecture.md](architecture.md#screen-and-input).

## What an app is given

The shell starts the app's program from inside the app's folder, as an unprivileged user, with:

| | |
|---|---|
| `T48_SOCKET_FD` | number of an open Unix stream socket to the shell (3 today; read the variable, do not assume) |
| `T48_BUFFER_FD` | number of an open file to map as the screen (4 today) |
| `T48_APP_ID` | the id from the manifest |
| `T48_APP_DATA` | a folder the app may write to (also `HOME`) |
| `T48_FONTS_FD` | level 2: number of an open file holding the shell's fonts (5 today), to map read-only. See [The theme](#the-theme) |
| `T48_FLIP_FD` | level 3: number of an open file holding the flip buffers (6 today). See [Flipping](#flipping) |
| `T48_SOUND_FD` | level 4, with the `audio-out` permission: number of an open file holding the sound ring (7 today). See [Sound](#sound) |

The app has no access to the framebuffer, the input devices or the sound hardware and does not need any.

**USB sticks.** Each FAT or exFAT stick in the socket is at `/run/media/NAME`, read-only: NAME is the volume label with anything but `A-Za-z0-9._-` made `_`, leading `._-` and trailing `_` dropped; or the device (`sda1`) when that leaves nothing or another stick has that label. It appears within a second of going in and goes when it is taken out, so list `/run/media` again when you need it rather than once at start. No permission is needed.

## The screen

Map `T48_BUFFER_FD` shared and writable for the size the shell gives in HELLO. It is a picture of the whole screen: one 32-bit word per pixel, `0x00RRGGBB`, rows `stride` bytes apart. Draw into it whenever you like, then send DAMAGE for the rectangle that changed; the shell copies that rectangle to the real screen. Nothing reaches the screen without a DAMAGE message. The size comes from HELLO: do not build it in.

**Which size.** 800 x 480, the T48S's screen, on every phone. On a phone with a smaller screen (the T46S: 480 x 272) the shell fits that picture to the screen, each screen pixel the average of the picture's pixels under it, so an app written for the T48S runs there unchanged, small and a little soft. An app whose manifest says `"screen": "any"` (level 6) is given the screen's own size instead and must lay itself out from the width and height in HELLO; its picture is then shown pixel for pixel. The format is XRGB8888 either way: the shell packs it to the T46S's 16-bit panel, and turns it, as it copies.

**What there is to press with.** HELLO's `flags` (level 6) say whether the screen takes touches (bit 0) and whether there are four soft keys under it (bit 1); with libt48, `t48_has_touch()` and `t48_has_softkeys()`. A phone without touch has the soft keys. An app that uses them says what they do along the bottom edge, each label above its key: `t48_ui_softkeys()` draws them as the shell's own screens do. A shell older than level 6 sends HELLO without `flags`: take that as touch and no soft keys.

## Messages

Each message is a 4-byte header then the payload. All numbers are little endian.

```
u16 type
u16 len      payload bytes that follow
```

Skip any type you do not know: that is how later levels stay compatible.

Shell to app:

| Type | Name | Payload |
|---|---|---|
| 1 | HELLO | `u16 api, u16 width, u16 height, u16 format (0 = XRGB8888), u32 stride, u32 size`, and from level 6 `u32 flags` (1: touch, 2: soft keys). Always first. An app takes as much of it as came: 16 bytes before level 6, 20 since. |
| 2 | SETTING | `key=value` text, no terminator. One per setting in the manifest, before START. |
| 3 | START | none. Map the buffer, answer READY, draw. |
| 4 | KEY | `u16 code, u8 state (1 pressed, 0 released), u8 0`. To a level 3 app: `u16 code, u8 state, u8 0 x 5, u64 time` |
| 5 | TOUCH | `u8 state (0 up, 1 down, 2 move), u8 0, i16 x, i16 y` in screen pixels. To a level 3 app: the same, then `u16 0, u64 time` |
| 6 | PAUSE | none. The app is off the screen. |
| 7 | RESUME | none. The app is in front again: redraw everything. |
| 8 | QUIT | none. Exit within one second. |
| 9 | THEME | level 2: the look, as below. Before START, and again whenever the owner changes it. |
| 10 | FLIPINFO | level 3: `u16 count, u16 0, u32 size, u32 period`. Before START. See [Flipping](#flipping) |
| 11 | FLIPPED | level 3: `u16 buffer, u16 0, u32 frame, u64 time`. The answer to FLIP |

App to shell:

| Type | Name | Payload |
|---|---|---|
| 0x81 | READY | `u16 api`: the level the app was written for |
| 0x82 | DAMAGE | `u16 x, u16 y, u16 w, u16 h` |
| 0x83 | PAUSED | none. Optional answer to PAUSE: "freeze me now". |
| 0x84 | | not used |
| 0x85 | FLIP | level 3: `u16 buffer, u16 0`. Show this flip buffer at the next refresh |
| 0x86 | LAMPS | level 5: `u8 set, u8 release, u8 level[4]`. See [Lamps](#lamps) |
| 0x87 | LAMP_PATTERN | level 5: `u8 mask, u8 flags, u16 count`, then `count` keys of `u16 ms, u8 level, u8 0` |

Key codes: digits, `*` and `#` are their ASCII characters; up 0x100, down 0x101, left 0x102, right 0x103, OK 0x104, back 0x105. Level 3: the four soft keys of a phone that has them, left to right, 0x110 to 0x113.

Every `time` is in microseconds on `CLOCK_MONOTONIC`, the clock the app's own `clock_gettime` reads. The shell sends the level 3 forms of KEY and TOUCH, and the soft keys, only to an app that answered READY with 3 or more.

## Flipping

Level 3, for games and anything else that draws the whole screen many times a second. `T48_FLIP_FD` holds `count` pictures of the screen, one after another, `size` bytes each, laid out as HELLO says. Map it shared and writable. Draw a whole picture into one buffer and send FLIP: the shell puts it on the screen at the next refresh, all at once, so nothing tears. FLIPPED comes back when it is there.

- On the T48S the screen is pointed at the buffer itself. Nothing is copied, and the buffers are ordinary cached memory, quick to read back.
- Do not draw in a buffer between its FLIP and the FLIPPED of the next one: it is on the screen, or about to be. With three buffers there is always one free, so the next picture can be started at once.
- One FLIP at a time. A FLIP sent before the last one's FLIPPED replaces it; both are answered.
- While an app is flipping, the screen stays bright.
- `frame` counts the screen's refreshes. A step of more than one between two FLIPPEDs is a refresh the app missed. `period` in FLIPINFO is one refresh in microseconds: 14746 on the T48S (67.8 a second), 16667 in the simulator. The rate an app can hold is that, or a half or a third of it.
- A buffer holds what was last drawn in it, two or three pictures ago, not the last picture.
- DAMAGE goes back to the shared buffer: the shell shows all of it and stops flipping. FLIP starts again.
- After PAUSE a FLIP is answered at once and not shown. When the app is back in front the shell shows its last flipped picture itself.
- `count` 0, or no FLIPINFO at all (a level 2 shell): draw in the shared buffer and send DAMAGE.

On a screen with no spare frames of its own (the T46S, the simulator, and any picture the shell has to fit) the buffers are a file in memory, a FLIP is one copy to the screen and FLIPPED comes sixty times a second. At the T46S's own 480 x 272 that copy is 130,000 pixels packed to 16 bits, a few milliseconds; a fitted 800 x 480 picture costs several times that, which is the best reason for a game to say `"screen": "any"`.

The T48S's screen is 800 x 480 at 32 bits and nothing smaller is on offer: the LCD controller cannot stretch a picture. A game that draws 400 x 240 doubles each pixel into the flip buffer itself (`t48_blit_scaled`). Why there is no 16-bit mode is in [architecture.md](architecture.md#flipping).

What it costs on the T48S, measured with `libt48/examples/fliptest`. Memory moves at about 160 MB a second, so filling the screen once is 10 ms of the 14.7 ms a refresh lasts:

| | |
|---|---|
| a bar redrawn, flipped | 67.8 a second, none missed |
| every pixel redrawn, flipped | 67.8 a second, none missed, 10.7 ms of each frame spent drawing |
| 400 x 240 made and doubled, flipped | 67.8 a second, none missed, 9.6 ms |
| every pixel redrawn where the shell must copy each flip | 43 a second, refreshes missed |

## Sound

Level 4, for an app whose manifest has the `audio-out` permission. One format: 16000 samples a second, one channel, signed 16-bit, which is what the phone's codecs run at. `T48_SOUND_FD` is a ring of samples in memory shared with the audio service ([audiod.md](audiod.md#an-apps-sound)). Map all of it shared and writable (`fstat` gives the size). Nothing is sent and nothing is copied on the way: the app writes samples, the service takes them out 20 ms at a time.

```
u32 magic       0x53383454 ("T48S")
u32 rate        16000
u32 frames      samples the ring holds: 4096
u32 data        where the samples start, in bytes
u32 rd          the service's: samples taken
u32 state       the service's: 0 off, 1 playing, 2 held
u32 delay       the service's: samples from the ring to the loudspeaker
u32 dry         the service's: times the ring ran empty while playing
u32 wr          the app's: samples written
u32 waiting     the app's: not 0 asks to be woken when rd moves
```

- **To play:** put samples at `data[wr % frames]` onwards, then add the number to `wr`. `wr` and `rd` count up for ever and wrap at 2^32; `wr - rd` is what is waiting and must not pass `frames`. The app writes the samples, `wr` and `waiting`, and nothing else.
- **To wait for room:** set `waiting` to 1 and sleep in `futex(FUTEX_WAIT)` on `rd`, with a timeout. Or look again later: the service takes 320 samples every 20 ms.
- **How much to keep waiting:** 320 samples or more whenever the service comes for them, or there is a gap. A sound starts when 320 are there, or after 60 ms if it is shorter than that.
- **Delay:** what is waiting, plus `delay`, plus 10 ms on average for the service's next take. Measured on the T48S with 640 samples waiting at most: 65 ms from a write to the loudspeaker.
- **Stop writing and there is silence.** Nothing is played twice. `dry` counts each time the ring ran out.
- **Where it is heard:** the loudspeaker, at the owner's Apps volume. The volume keys change that volume while the app is in front. The [loudspeaker's limit](audiod.md#the-loudspeakers-limit) applies: a full-scale sample at volume 15 is on it, so sound from an app is quiet beside a desktop loudspeaker, and nothing an app writes can be louder.
- **Calls win.** `state` 2: the phone is ringing, playing a tone, or in a call. Samples are taken at the same rate and not played. Lifting the handset, Speaker and Headset start a call as they always do; the app is paused.
- **Off the screen is silent,** from PAUSE on, and what was waiting is dropped. `state` 0: nothing is taken. Throw samples away at the rate they would have played, so a program that keeps time by its sound still does. `state` is also 0 when there is no audio service, as in the simulator.
- **No `T48_SOUND_FD`:** no permission, a level 3 shell, or a kernel without memfd (Linux 3.6). Carry on without sound.

With libt48: `t48_sound_open()`, then `t48_sound_write()` (waits for room) or `t48_sound_put()` and `t48_sound_free()` (never wait), `t48_sound_latency_us()`, `t48_sound_buffer()` for how much may wait (1024 samples unless told), `t48_sound_close()`. They do all of the above, the throwing away included, so an app need not look at `state`. `t48_resample()` turns 8000 to 96000 Hz, mono or stereo, into the ring's format in whole numbers: a mean when going down, a straight line going up. The smallest program that plays anything is [`libt48/examples/tones`](../libt48/examples/tones/tones.c).

In the simulator there is no sound: writing keeps the right pace and the samples go nowhere. To hear or measure them, run `t48-audiod --spk out.wav` and give the simulator `--audio` with its control socket.

## Lamps

Level 5, for an app whose manifest has the `lamps` permission, while it is in front.

| Lamp | | |
|---|---|---|
| 0 | message | red, top right of the case. On or off: on from level 128. Slow: twenty changes a second at most, each costing the phone 7 ms |
| 1 | Headset key | green, 255 levels |
| 2 | Mute key | red, under Headset, 255 levels |
| 3 | Speaker key | green, under Mute, 255 levels |

A mask has bit `n` for lamp `n`. Levels are 0 to 255 as the eye takes them: equal steps look equal, so a fade is a plain count ([how](../kernel6/README.md#key-lamps)).

- **LAMPS** sets each lamp in `set` to its `level`, now, and gives each lamp in `release` back to the phone. Send it as often as you draw, to keep time with music.
- **LAMP_PATTERN** hands the shell a fade to run by itself, the same on every lamp in `mask`. From where the lamp is, it goes in a straight line to each key's level over that key's `ms`, one key after another; `ms` 0 is a jump. At the end the lamp stays at the last level, or with `flags` 1 goes round again from the first key. 1 to 32 keys. The shell steps it fifty times a second.
- A new LAMPS or LAMP_PATTERN for a lamp replaces what it was doing.
- **A lamp the app has set is the app's** until it is released. The phone keeps track of what it wants on it meanwhile (mute, a missed call, a message waiting) and shows that the moment it has the lamp back.
- **Calls win.** PAUSE takes every lamp back at once, and that is what a call does to an app. So do QUIT and the app's end. After RESUME the lamps are the phone's: set them again.
- Without the permission both messages are ignored. A lamp the phone has not got is ignored.

With libt48: `t48_lamp()`, `t48_lamps_set()`, `t48_lamp_pattern()`, `t48_lamp_fade()`, `t48_lamp_pulse()`, `t48_lamps_release()`; `t48_lamps_ok()` is 0 on an older shell, where the rest do nothing. [`libt48/examples/lamps`](../libt48/examples/lamps/lamps.c) uses all of them. The simulator has no lamps and says what would be lit: `lamp.headset` and so on ([shell.md](shell.md#the-simulator)).

What it costs on the T48S: a lamp at a level between off and full takes 0.5% of the processor, three at different levels 1%, 2% at worst. A pattern on three lamps takes the shell 1% more; fifty LAMPS a second, 2%.

## The theme

The owner picks one of three themes (Settings, Screen, Theme, or the web setup page: [design](design/README.md)). The shell tells each app which in a THEME message before START, and again at once when it changes; an app that is frozen finds the message waiting ahead of RESUME. An app that takes its colours from it looks like the rest of the phone. With libt48 that is `t48_theme_now()`, the event `T48_EV_THEME` (draw everything again), and nothing else: the fonts, the keyboard and the `t48_ui_*` parts follow by themselves.

THEME's payload (`struct t48_msg_theme`); colours are `u32 0x00RRGGBB`:

```
char name[16]        "switchboard", "directory" or "twotone", NUL padded
char font[16]        the family to use from the font file: "b612", "plex", "figtree"
u8 radius            corners of buttons and boxes, in pixels
u8 key_radius        corners of a keypad key; 100 is a round key
u8 edge              how a key or plain button is finished: 0 flat, 1 outlined, 2 a lip under it
u8 rows              rows of a list: 0 boxes, 1 ruled, 2 slightly lighter boxes
u8 head_rule         1: a rule under the header
u8 dark              1: light ink on a dark ground
u8 0, 0
u32 bg, panel, line                  the ground, a quiet box on it, a rule
u32 text, dim, faint                 ink on the ground
u32 key, on_key, on_key_dim          a key or plain button, and ink on it
u32 edge_colour                      the outline, when edge is 1
u32 accent, on_accent                lit: switched on, or wants attention
u32 good, on_good, bad, on_bad       go and stop
u32 sel, on_sel, on_sel_dim          the selected row
u32 head, on_head, on_head_dim       the header
u32 first, on_first                  the one button that matters most
u32 ground_ring, ground_call, ground_hold    the shell's own ground while a call rings, is up, is held
```

A later level may add to the end: take the part you know. `accent` is a fill, not an ink: in Directory it is the yellow of the header, which cannot be read on paper.

The font file (`T48_FONTS_FD`) holds every family the shell has, so an app need not carry them (libt48 has B612 built in and takes the others from here). Offsets are from the start of the file:

```
char magic[4]        "T48F"
u16 version          1
u16 families
then for each family:
  char name[16]
  8 faces, each:     i16 ascent, i16 height, u32 glyphs, u32 coverage
```

The faces are, in order: 12 px fixed width, 15 px, 19 px, 19 px bold, 15 px fixed width, 26 px bold, 40 px bold, 104 px bold (digits, colon, dot, minus and space only). `glyphs` is the offset of 95 records, for ASCII 32 to 126: `i16 adv, w, h, xoff, yoff, u16 0, u32 off`. A glyph is `w` x `h` bytes of coverage (0 to 255), row by row, at `coverage + off`; draw it `xoff`, `yoff` from the pen and the top of the line, then move the pen `adv`.

An app written for level 1 skips THEME and never looks at the font file: it keeps its own colours and B612. An app built with a level 2 libt48 runs on a level 1 shell as Switchboard. So `"api": 1` in the manifest is still right for an app that only follows the theme; ask for 2 only if the app cannot do without it. The same goes for level 3: an app that flips when it can and draws by DAMAGE when it cannot (`t48_flip_start()` gives NULL) may still say 1 or 2. And for level 4: an app that is content to be silent on an older phone (`t48_sound_open()` gives -1) need not ask for 4. Nor for 5, if it can do without lamps.

## Life of an app

1. The shell sends HELLO, the settings, THEME, START. The app answers READY and sends its first DAMAGE. An app that has not answered within 5 seconds is killed.
2. While in front it gets KEY and TOUCH, each press and each release, as the kernel reports them (it reads the keys every 20 ms and the touchscreen every 10 ms). Apps get digits, `*`, `#`, arrows, OK and back, and at level 3 the soft keys. Back is a short press on X, sent when X is released. **A long press on X is Home.** It is never sent, the shell acts on it whatever the app is doing, flipping or not, and it is the one way out that an app cannot take away. The phone keys (hook, Speakerphone, Headset, Mute, Volume, Hold, Transfer, Redial, Message) are never sent.
3. PAUSE arrives when the owner goes Home or a call takes the screen. Its sound stops at that moment, and its [lamps](#lamps) go back to the phone. The app should stop drawing, close network connections and forget any keys it thinks are held. One second later (or at once if it sends PAUSED) it is frozen with SIGSTOP. RESUME follows a SIGCONT; the shell repaints the whole screen from the buffer at that moment, so an app whose picture is still right has nothing to send.
4. QUIT means go. An app still there after one second is killed. An app may also just exit when it is finished: exit status 0 returns to the launcher quietly, anything else (or a crash) shows the owner a message.
5. An app that stops reading its socket for so long that the shell cannot queue more events is killed.

Only one app runs at a time. Opening another closes the first.

## The manifest

An installed app is a folder holding `manifest.json`, the program and an icon. The shell looks in `/data/apps` and `/usr/share/t48/apps`.

```json
{
  "id": "clock",
  "name": "Clock",
  "version": "0.1.0",
  "description": "A clock, and the last key or touch",
  "api": 1,
  "exec": "clock",
  "icon": "icon.ppm",
  "category": "General",
  "permissions": ["network"],
  "settings": [
    { "key": "title", "label": "Title", "type": "text", "default": "Clock" }
  ]
}
```

- `id` (a-z, 0-9, `-`, `.`, `_`), `exec` (a path inside the folder) and `api` are required. An app asking for a higher API level than the shell speaks is not listed.
- `"screen": "any"` (level 6): the app lays itself out for whatever size HELLO gives, and gets the screen's own ([The screen](#the-screen)). Without it the app is promised 800 x 480. It needs no higher `api`: a shell older than level 6 ignores the word, and its screen is 800 x 480 anyway.
- `name`, `version` and `description` are shown to the owner; `t48-pack` (in the SDK) needs `name` and `version`, and copies `description` into the feed.
- `category`: one of `General`, `Games`, `Radio`, `Tools`, `Media`, as written. The app manager groups apps by it ([apps.md](apps.md)); without one the app is General. `t48-pack` refuses any other value and copies it into the feed. Older firmware ignores it.
- `icon` is a binary PPM (P6) up to 96 x 96 for now; without one the launcher shows the name's first letter. PNG will follow when libt48 can read it.
- `permissions` is shown to the owner by the app manager ([apps.md](apps.md)). `audio-out` is enforced: without it the app is given no [sound](#sound). `lamps` is enforced: without it the app cannot set a [lamp](#lamps). `network` is not enforced yet.
- `settings`: each has a `key`, a `label`, a `type` (`text`, `number`, `bool` or `password`) and a `default`, and reaches the app as a SETTING message. The value is the manifest's default unless the owner has changed it in Settings, App settings ([settings.md](settings.md#an-apps-settings)), which keeps it in `/data/config/apps/<id>.conf`.
