# CrossPoint-Mod-DX34

Firmware for the **Xteink X4 / DX34-class ESP32-C3 e-paper reader** — a DX34-focused fork of CrossPoint.

![](./docs/images/cover.jpg)

## What this is

A reading-first custom firmware for the X4/DX34: faster book access, richer EPUB controls, managed sleep wallpapers, Wi-Fi transfer, and OTA updates. Prioritises DX34 UX over upstream parity — see [docs/differences-from-upstream.md](./docs/differences-from-upstream.md).

## Install

> Back up your SD card before flashing. Books are not modified, but reading progress from other firmwares is not migrated.

1. Connect the device by USB-C and wake it.
2. Open [xteink.dve.al](https://xteink.dve.al/) and flash the latest [release](https://github.com/diogo7dias/crosspoint-reader-DX34/releases).

Manual: `pio run -t upload`. Build, flash, and host-test setup in [docs/development.md](./docs/development.md).

## Screenshots

| Crisp | Dark | Bionic |
|:---:|:---:|:---:|
| <img src="./docs/images/screenshots/reader-crisp.jpg" width="240"/> | <img src="./docs/images/screenshots/reader-dark.jpg" width="240"/> | <img src="./docs/images/screenshots/reader-bionic.jpg" width="240"/> |

| Home | In-book menu |
|:---:|:---:|
| <img src="./docs/images/screenshots/home-screen.jpg" width="240"/> | <img src="./docs/images/screenshots/in-book-menu.jpg" width="240"/> |

## Features

- **[EPUB reader](./docs/reader.md)** — in-book menu, 16 Reading Themes per book, highlights, 20 bookmarks per book, footnotes, live text settings
- **[Fonts](./docs/fonts.md)** — ChareInk, Bookerly, Vollkorn at 12–17 pt R/B/I; Bitter at 10/12/14/16 R/B/I; Galmuri (Korean) regular; user-uploadable custom `.bin` font; Unifont fallback
- **[Sleep wallpapers](./docs/wallpapers.md)** — nine modes, managed `/sleep` folder, favorites with auto-trim, in-book triage, native `.pxc` pre-dithered format ([converter](https://crosspoint-pxc-converter.pages.dev/))
- **[Device controls](./docs/controls.md)** — button remapping, auto-sleep, refresh frequency, custom boot image, orientation
- **[Wi-Fi & web server](./docs/webserver.md)** — file manager (upload/download/rename/move/preview), WebDAV mount, WebSocket fast upload, in-browser sleep-wallpaper converter, custom font upload, OPDS, Calibre-Web, KOReader progress sync, OTA from GitHub releases with downgrade guard
- **Home & library** — recents (up to 100), file browser with search and QR sharing, Classic and Single-Cover layouts

## Formats

EPUB 2/3 (primary, all advanced features), TXT, MD, and pre-rendered XTC/XTG/XTH. Details in [docs/file-formats.md](./docs/file-formats.md).

## Scope

- EPUB is the primary target; TXT and XTC are supported but do not share the full in-book workflow.
- English-first.
- ESP32-C3 RAM caps sleep playlists and folder browse size.
- GPIO-only — Bluetooth/BLE input is not supported.

## Support and project links

- Ideas: [GitHub Discussions](https://github.com/diogo7dias/crosspoint-reader-DX34/discussions/categories/ideas)
- Bugs: [GitHub Issues](https://github.com/diogo7dias/crosspoint-reader-DX34/issues)
- Governance: [GOVERNANCE.md](./GOVERNANCE.md)
- Tip jar: [Ko-fi](https://ko-fi.com/d7d7m)

## Attribution

This project is independent and unaffiliated with Xteink.

DX34 is a fork of **CrossPoint**, originally created by [@daveallie](https://github.com/daveallie) and the CrossPoint team.

- [CrossPoint (upstream)](https://github.com/crosspoint-reader/crosspoint-reader) — original firmware by @daveallie and contributors
- [atomic14/diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader)
