# The app manager

Issue [#22](https://github.com/M0LTE/yealink-t48s/issues/22). Design: [architecture.md](architecture.md#the-apps-repository). Source: [`shell/pkg.c`](../shell/pkg.c) (`t48-app`), [`shell/settings.c`](../shell/settings.c) (the screens).

Open **Apps**, then the last tile, **Get apps** (also Settings, Apps; and the Apps page of the web setup page, [web.md](web.md)). It lists what is installed and what the feed offers; pick one to install, update, open, change its settings or remove it.

| | |
|---|---|
| ![the list](img/apps-list-live.png) | ![installing](img/apps-installing.png) |
| ![an installed app](img/apps-detail.png) | ![removing](img/apps-remove.png) |
| ![a package that fails its check](img/apps-checksum.png) | ![the launcher afterwards](img/apps-launcher.png) |

- Apps are grouped by category under headings: General, Games, Radio, Tools, Media, in that order, installed apps first in each. With only one category there are no headings. The keys step over the headings. An app without a category, or with one this firmware does not know, is General. An installed app's category is its manifest's, else the feed's.
- Each app's page shows its version, its size (download, and on the phone once installed) and what it may use (its manifest's permissions).
- Remove asks first, and takes the app's own data and settings with it. Update keeps them.
- Apps that are part of the firmware (the clock) cannot be removed.
- Installing, updating or removing the app that is open closes it first.
- The feed address is a setting on the same screen (`/data/config/feed`, [settings.md](settings.md#the-files)). The standard one is `https://m0lte.github.io/yealink-t48s-apps/index.json`, published by [M0LTE/yealink-t48s-apps](https://github.com/M0LTE/yealink-t48s-apps).

| | |
|---|---|
| ![grouped by category](img/apps-categories.png) | ![the same on the T46S](img/t46s-apps-categories.png) |

## What is checked before an app is installed

1. The feed's address must be `https://`. curl does the download, with mbedTLS and the image's CA bundle.
2. The app's `api` in the feed must not be higher than the level this shell speaks (4 today). A newer app is listed as "needs newer firmware" and has no Install button.
3. The package must have exactly the `size` and `sha256` the feed gives. If not it is thrown away and the owner is told.
4. The package must hold one folder, named with a proper app id, with only plain files and folders inside it: no links, no devices, nothing that climbs out with `..`. The shell unpacks the tar itself rather than trusting a tar program with that.
5. The manifest inside must read properly, have the id the feed said, an API level this shell speaks, and a program to run.

Only then is the app moved into `/data/apps` with a rename, so a half-installed app is never visible, and a failed update leaves the old version in place.

## The feed

`index.json`, as `t48-pack --index` writes it ([sdk/README.md](../sdk/README.md#packages-and-feeds)). Nothing had to change: `size` and `sha256` were already there. What the phone reads:

| | |
|---|---|
| `feed` | must be 1 |
| `apps[].id`, `url` | needed. `url` is relative to the index, or a full address |
| `apps[].size`, `sha256` | needed to install: an entry without them is listed but refused |
| `apps[].api` | refused if higher than the shell's |
| `apps[].name`, `version`, `description`, `permissions` | shown to the owner |
| `apps[].category` | the group it is listed under; General when absent (issue [#30](https://github.com/M0LTE/yealink-t48s-apps/issues/30)). Firmware before it ignores the field |

## On the command line

```
t48-app list                  # installed, and what the feed has, with each app's category
t48-app install hello         # from the feed
t48-app install /tmp/my-0.1.0.t48app    # a file copied to the phone: no checksum to check it against
t48-app update                # everything the feed has a newer version of
t48-app update hello
t48-app remove hello
t48-app check                 # how many installed apps the feed has newer: the daily check's question
t48-app --feed https://example.org/apps/index.json list
```

Each time `t48-app` reads the feed it keeps what the feed offers in `/data/phone/app-offers`. The home screen and the web setup page count updates from that against what is installed, so the count goes as soon as an app is updated or removed. The daily check ([install.md](install.md#the-daily-check)) runs `t48-app check` once a night; nothing is installed by itself.

`t48-app` is the shell's own program under another name. The Apps screen runs it as a separate process and reads its progress, so a slow download never holds up the screen or a call.

## Testing

- `make -C shell check` runs [`tests/05-apps.script`](../shell/tests/05-apps.script) against two small feeds made on disk by [`tests/mkfeeds.sh`](../shell/tests/mkfeeds.sh): install, open, the app's settings, an update that keeps them, remove; and the refusals (an app for API level 2, a package one byte wrong, a package that climbs out of its folder, one holding a link, a feed address that is not https, removing the clock, a feed that is not there).
- Categories: [`tests/23-app-categories.script`](../shell/tests/23-app-categories.script) and the T46S's [`tests/small/09-app-categories.script`](../shell/tests/small/09-app-categories.script) against a feed of apps in several categories, one only in the feed and one this firmware does not know; `webconfig/tests` the same on the web page; [`tools/test-pack.sh`](../tools/test-pack.sh) the packer's check and index.
- `make -C shell check-live RUNNER=qemu-arm` uses the real feed: installs hello, links and waterfall from the Apps screen and starts each one (they are the phone's ARM programs, hence qemu).
- On the phone: below.

## On the phone (2026-11-23)

Done on the real phone with injected touches and keys (`tools/phone-input.sh`), nobody at it; the pictures are read back from its framebuffer.

- Get apps lists the real feed over HTTPS in under a second.
- Hello and Node links were installed by touch, Waterfall by keys: about two seconds each, checksum checked. All three then open from the launcher, each as its own user, and the waterfall and the links screen show live data.
- Hello was removed (it asks first; its data and settings went with it) and installed again. `t48-app update` says everything is up to date.

| | |
|---|---|
| ![the list on the phone](img/apps-on-phone-list.png) | ![installing on the phone](img/apps-on-phone-installing.png) |
| ![the launcher on the phone](img/apps-on-phone-launcher.png) | ![removing on the phone](img/apps-on-phone-remove.png) |
| ![waterfall, installed from the feed](img/apps-on-phone-waterfall.png) | ![links, installed from the feed](img/apps-on-phone-links.png) |

Two faults in the image showed up only here and are fixed: curl had no CA bundle, and `/dev/random` blocked for ever ([buildroot/README.md](../buildroot/README.md)). The phone's kernel also has no `flock`, so `t48-app` keeps a small lock file instead.

## Not done

- One feed at a time. Several feeds, and signed feeds, are for later.
- While `/data` is in RAM (#29), installed apps are lost at power off. Fetching a list of apps again at boot is not written.
- The image's `/dev/random` is pointed at the non-blocking pool so that HTTPS works at all on this kernel (see `rcS`); its randomness just after boot is weak until a seed can be kept across boots (#29).
