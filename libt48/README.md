# libt48

The small static C library that apps for the T48 firmware link against: drawing, built-in fonts, a JSON reader, a WebSocket client, and the app side of the [app protocol](../docs/app-protocol.md). The whole API is in [`include/t48.h`](include/t48.h), with a minimal app at the top of it; [`examples/clock`](examples/clock) is a complete one.

```
make                                               # this PC, into build/
make CC=$HOME/src-build/t48s/bin/armcc O=build-arm # the phone
```

Run an app on a PC with the simulator: see [docs/shell.md](../docs/shell.md#the-simulator).

The WebSocket client (plain `ws://`) came from the old `native/waterfall.c`; `wss://` will follow with mbedTLS. Not written yet: PNG reading, the on-screen keyboard.

Licence: AGPL-3.0-or-later (see `LICENSE` here). The fonts are DejaVu (Bitstream Vera licence), rendered by `tools/mkfont.py`.
