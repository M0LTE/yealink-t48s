# The app protocol

API level 1. This is the contract between the shell (`t48-shell`) and an app. `libt48` wraps it for C programs, but an app in any language can speak it directly. The C definitions are in [`libt48/include/t48_proto.h`](../libt48/include/t48_proto.h). The design reasons are in [architecture.md](architecture.md#screen-and-input).

## What an app is given

The shell starts the app's program from inside the app's folder, as an unprivileged user, with:

| | |
|---|---|
| `T48_SOCKET_FD` | number of an open Unix stream socket to the shell (3 today; read the variable, do not assume) |
| `T48_BUFFER_FD` | number of an open file to map as the screen (4 today) |
| `T48_APP_ID` | the id from the manifest |
| `T48_APP_DATA` | a folder the app may write to (also `HOME`) |

The app has no access to the framebuffer or the input devices and does not need any.

## The screen

Map `T48_BUFFER_FD` shared and writable for the size the shell gives in HELLO. It is a picture of the whole screen: one 32-bit word per pixel, `0x00RRGGBB`, rows `stride` bytes apart. Draw into it whenever you like, then send DAMAGE for the rectangle that changed; the shell copies that rectangle to the real screen. Nothing reaches the screen without a DAMAGE message. The size comes from HELLO (800 x 480 on the T48S): do not build it in.

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
| 1 | HELLO | `u16 api, u16 width, u16 height, u16 format (0 = XRGB8888), u32 stride, u32 size`. Always first. |
| 2 | SETTING | `key=value` text, no terminator. One per setting in the manifest, before START. |
| 3 | START | none. Map the buffer, answer READY, draw. |
| 4 | KEY | `u16 code, u8 state (1 pressed, 0 released), u8 0` |
| 5 | TOUCH | `u8 state (0 up, 1 down, 2 move), u8 0, i16 x, i16 y` in screen pixels |
| 6 | PAUSE | none. The app is off the screen. |
| 7 | RESUME | none. The app is in front again: redraw everything. |
| 8 | QUIT | none. Exit within one second. |

App to shell:

| Type | Name | Payload |
|---|---|---|
| 0x81 | READY | `u16 api`: the level the app was written for |
| 0x82 | DAMAGE | `u16 x, u16 y, u16 w, u16 h` |
| 0x83 | PAUSED | none. Optional answer to PAUSE: "freeze me now". |
| 0x84 | | kept for asking for an audio stream, in a later level |

Key codes: digits, `*` and `#` are their ASCII characters; up 0x100, down 0x101, left 0x102, right 0x103, OK 0x104, back 0x105.

## Life of an app

1. The shell sends HELLO, the settings, START. The app answers READY and sends its first DAMAGE. An app that has not answered within 5 seconds is killed.
2. While in front it gets KEY and TOUCH. Apps get digits, `*`, `#`, arrows, OK and back. Back is a short press on X, sent when X is released. A long press on X is Home and is never sent. The phone keys (hook, Speakerphone, Headset, Mute, Volume, Hold, Transfer, Redial, Message) are never sent.
3. PAUSE arrives when the owner goes Home or a call takes the screen. The app should stop drawing, close network connections and forget any keys it thinks are held. One second later (or at once if it sends PAUSED) it is frozen with SIGSTOP. RESUME follows a SIGCONT; the shell repaints the whole screen from the buffer at that moment, so an app whose picture is still right has nothing to send.
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
  "permissions": ["network"],
  "settings": [
    { "key": "title", "label": "Title", "type": "text", "default": "Clock" }
  ]
}
```

- `id` (a-z, 0-9, `-`, `.`, `_`), `exec` (a path inside the folder) and `api` are required. An app asking for a higher API level than the shell speaks is not listed.
- `name`, `version` and `description` are shown to the owner; `t48-pack` (in the SDK) needs `name` and `version`, and copies `description` into the feed.
- `icon` is a binary PPM (P6) up to 96 x 96 for now; without one the launcher shows the name's first letter. PNG will follow when libt48 can read it.
- `permissions` is shown to the owner by the app manager ([apps.md](apps.md)); nothing enforces it yet.
- `settings`: each has a `key`, a `label`, a `type` (`text`, `number`, `bool` or `password`) and a `default`, and reaches the app as a SETTING message. The value is the manifest's default unless the owner has changed it in Settings, App settings ([settings.md](settings.md#an-apps-settings)), which keeps it in `/data/config/apps/<id>.conf`.
