# libt48

The small static C library that apps for the T48 firmware link against: drawing, built-in fonts, the theme and parts in its look (header, buttons, rows), a JSON reader, a WebSocket client, an on-screen keyboard, and the app side of the [app protocol](../docs/app-protocol.md). The whole API is in [`include/t48.h`](include/t48.h), with a minimal app at the top of it; [`examples/clock`](examples/clock) is a complete one.

```
make                                               # this PC, into build/
make CC=$HOME/src-build/t48s/bin/armcc O=build-arm # the phone
```

**Games.** `t48_flip_start()` and `t48_flip()` show whole pictures without tearing, in time with the screen, and on the phone without a copy; keys and touches carry the time they happened. [`examples/fliptest`](examples/fliptest) is the smallest program that does it, and the check for tearing. The simulator flips too, sixty times a second.

Run an app on a PC with the simulator: see [docs/shell.md](../docs/shell.md#the-simulator).

The WebSocket client (plain `ws://`) came from the old `native/waterfall.c`; `wss://` will follow with mbedTLS. The on-screen keyboard (`t48_kbd_*`) is the one the shell's settings use ([docs/settings.md](../docs/settings.md#the-keyboard)). Not written yet: PNG reading.

**Themes.** The owner picks one of three ([docs/design](../docs/design/README.md)). `t48_theme_now()` is the one in use: colours by what they are for (`bg`, `text`, `dim`, `good`, ...). Draw with those, draw everything again on `T48_EV_THEME`, and the app looks like the rest of the phone; the `t48_font_*` faces, the keyboard and the `t48_ui_*` parts follow the theme by themselves. An app carries only B612: the shell hands it the other families when it starts.

Licence: AGPL-3.0-or-later (see `LICENSE` here). The fonts are B612 and B612 Mono, IBM Plex Sans and IBM Plex Mono, and Figtree (each under the SIL Open Font Licence 1.1, in `fonts/` with its licence text: `OFL-B612.txt`, `OFL-IBMPlex.txt`, `OFL-Figtree.txt`), rendered by `tools/mkfont.py`.
