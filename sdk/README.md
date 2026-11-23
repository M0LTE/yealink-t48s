# The app SDK

Everything needed to build apps for the T48 firmware without this repository: the cross compiler and musl for the phone (Buildroot's relocatable SDK), libt48 and its headers, mbedTLS, the PC simulator, a packaging tool and shared make rules. Issue [#21](https://github.com/M0LTE/yealink-t48s/issues/21). Apps themselves live in [M0LTE/yealink-t48s-apps](https://github.com/M0LTE/yealink-t48s-apps), which has a guide to writing one.

## Getting it

The newest build is always at the same address, on the rolling pre-release `sdk-latest`:

```
curl -fsSLO https://github.com/M0LTE/yealink-t48s/releases/download/sdk-latest/t48s-sdk.tar.gz
curl -fsSLO https://github.com/M0LTE/yealink-t48s/releases/download/sdk-latest/t48s-sdk.tar.gz.sha256
sha256sum -c t48s-sdk.tar.gz.sha256
tar -xzf t48s-sdk.tar.gz                 # makes t48s-sdk/
t48s-sdk/relocate-sdk.sh                 # once, after unpacking or moving it
export T48_SDK=$PWD/t48s-sdk
```

Each firmware release (a `v*` tag) also carries `t48s-sdk.tar.gz`, built from the same commit as the firmware; use one of those when you want a fixed SDK. `sdk-latest` is updated by the build workflow when it is run by hand (`gh workflow run build.yml`) or for a release tag; its notes say which commit it came from. It runs on x86-64 Linux.

## What is in it

| | |
|---|---|
| `bin/arm-buildroot-linux-musleabi-gcc` | the cross compiler: ARM1176, soft-float, musl, static |
| `arm-buildroot-linux-musleabi/sysroot` | musl, libt48 (`-lt48`, `t48.h`), mbedTLS and the rest of the phone's libraries |
| `bin/t48-sim` | the PC simulator ([docs/shell.md](https://github.com/M0LTE/yealink-t48s/blob/main/docs/shell.md#the-simulator)) |
| `bin/t48-pack` | makes a `.t48app` from an app folder, and `index.json` for a feed |
| `share/t48/app.mk` | make rules: an app's Makefile is three lines |
| `share/t48/libt48/` | libt48's source, for building apps to run in the simulator |
| `share/t48/api` | the app protocol level this SDK speaks (2 added the theme: an app built with this SDK follows the owner's theme, and still runs on level 1 firmware) ([docs/app-protocol.md](https://github.com/M0LTE/yealink-t48s/blob/main/docs/app-protocol.md)) |
| `share/t48/VERSION` | the commit and date it was built from |

## An app's Makefile

```make
APP = hello
SRC = hello.c
include $(T48_SDK)/share/t48/app.mk
```

`make` builds the phone's app folder in `build/arm/hello` and the same for the PC in `build/host/hello`; `make package` makes `build/hello-VERSION.t48app`; `make shot` starts it in the simulator and saves `build/shot.png`; `make check` runs `tests/*.script` in the simulator.

## Packages and feeds

A `.t48app` is a tar.gz holding one folder named after the app's id, with its `manifest.json`, program and icon, so `tar -C /data/apps -xzf hello-0.1.0.t48app` installs it by hand. `t48-pack --index DIR` writes `DIR/index.json` for every package in `DIR`:

```json
{
  "feed": 1,
  "generated": "2026-11-23T12:00:00Z",
  "apps": [
    { "id": "hello", "name": "Hello", "version": "0.1.0", "api": 1,
      "description": "The smallest useful app", "permissions": [],
      "url": "hello-0.1.0.t48app", "size": 41234, "sha256": "..." }
  ]
}
```

`url` is relative to the address of `index.json`. A feed is just that file and the packages beside it on any web server.

## Building the SDK

CI does it (`.github/workflows/build.yml`): `buildroot/build.sh WORKDIR sdk` for Buildroot's part, then `sdk/assemble.sh` adds the rest.
