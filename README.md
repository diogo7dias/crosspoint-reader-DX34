# CrossPoint-Mod-DX34

Firmware for the **Xteink X4 / DX34-class ESP32-C3 e-paper reader**, maintained as a **DX34-focused fork** of CrossPoint.

![](./docs/images/cover.jpg)

---

## What this firmware is

`CrossPoint-Mod-DX34` is a custom build for people who want the X4/DX34 hardware to behave like a practical reading device first: faster access to books, stronger EPUB controls, configurable sleep wallpapers, Wi-Fi transfer, OTA updates, and full reader customization without leaving the book.

This fork keeps upstream credit and history. It does not aim for strict parity with upstream CrossPoint.

## How it differs from upstream

DX34 prioritizes its own UX and release cadence over upstream parity. Compared to mainline CrossPoint, this build ships a different font set (three reader families at six sizes each), a richer in-book workflow (Reading Themes, highlights, bookmarks, footnotes), a managed sleep wallpaper system with favorites and triage, a DX34-specific home screen, and extensive reader controls. More positioning notes live in [docs/differences-from-upstream.md](./docs/differences-from-upstream.md).

---

## Installation

> **Back up your SD card before flashing.** Book files are safe — the firmware does not modify your EPUBs, TXTs, or other content. But reading progress from another firmware is not migrated: DX34 stores book positions in its own format under `/.crosspoint/books/`. A backup lets you roll back if needed.

### Web flasher (easiest)

1. Connect the device by USB-C.
2. Wake or reboot it.
3. Open [xteink.dve.al](https://xteink.dve.al/).
4. Flash the latest firmware or a downloaded `.bin`.

### Releases

Release binaries are published at [DX34 releases](https://github.com/diogo7dias/crosspoint-reader-DX34/releases).

### Manual flashing

```sh
pio run -t upload
```

See [docs/development.md](./docs/development.md) for build, flash, serial monitoring, and host-test setup.

---

## At a glance

| Crisp mode | Dark mode | Bionic mode |
|:---:|:---:|:---:|
| <img src="./docs/images/screenshots/reader-crisp.jpg" width="250"/> | <img src="./docs/images/screenshots/reader-dark.jpg" width="250"/> | <img src="./docs/images/screenshots/reader-bionic.jpg" width="250"/> |

| Home screen | In-book menu |
|:---:|:---:|
| <img src="./docs/images/screenshots/home-screen.jpg" width="250"/> | <img src="./docs/images/screenshots/in-book-menu.jpg" width="250"/> |

---

## Features

- **[EPUB reader](./docs/reader.md)** — in-book menu, Reading Themes (16 per book), highlights and quotes, bookmarks (20 per book), footnote navigation, live text settings, configurable status bar
- **[Fonts](./docs/fonts.md)** — three reader families (ChareInk, Bookerly, Vollkorn) at six sizes (12–17 pt) with regular/bold/italic styles; Unifont fallback
- **[Sleep wallpapers](./docs/wallpapers.md)** — nine sleep modes, managed `/sleep` folder, favorites with auto-trim protection, in-book triage actions, native `.pxc` pre-dithered wallpapers (convert at <https://crosspoint-pxc-converter.pages.dev/> for sharper, faster, smaller wallpapers than `.bmp`)
- **[Device controls](./docs/controls.md)** — side and front button remapping, power button action, auto-sleep, refresh frequency, custom boot image, orientation
- **[Wi-Fi features](./docs/webserver.md)** — web file transfer with resume, OPDS browser, Calibre wireless, KOReader progress sync, OTA updates from GitHub
- **[Home screen and library](#home-screen-and-library)** — recents (up to 100), file browser with search, QR file sharing

## Supported book formats

| Format | Notes |
|--------|-------|
| **EPUB** (2 and 3) | Primary reading format — all DX34 advanced features live here. Inline JPEG and PNG, partial CSS, HTML entity decoding |
| **TXT / MD** | Plain-text reader with progress tracking |
| **XTC / XTCH** | Pre-rendered page format with chapter selection |

Advanced in-book features (themes, highlights, footnotes, live settings) are EPUB-only.

## Home screen and library

The home screen includes a recents list with reading progress (up to 100 books), entries for My Library, Recents, File Transfer, and Settings, and an optional OPDS entry when an OPDS server is configured. Two layouts are available: **Classic** (list of recent books) and **Single Cover** (large cover display).

Books are moved to `/recents/` when first opened, so they stay organized. Quote files (`_QUOTES.txt`) are preserved when a book is removed or deleted.

The **My Library** browser supports folder navigation, open/rename/move/delete, fuzzy search, BMP preview, and hidden-file toggle. Large folders paginate (up to 300 entries, or 1000 inside `/sleep`).

## Wireless

The Wi-Fi file-transfer mode starts a web server (STA or AP) accessible from any browser on the network. It supports upload with resume, full file management, search, settings, and a status page. A WebSocket on port 81 handles binary chunked transfers; UDP auto-discovery broadcasts help the web UI find the device. See [docs/webserver.md](./docs/webserver.md) and [docs/webserver-endpoints.md](./docs/webserver-endpoints.md).

**OPDS**, **Calibre wireless**, and **KOReader progress sync** (match by filename or by binary hash) are configured in Settings.

**OTA** checks GitHub releases over Wi-Fi. Version comparison prevents accidental downgrades, and dual OTA partitions let a failed update roll back safely.

## Storage

Runtime state and caches live under `/.crosspoint/` on the SD card (`settings.json`, `state.json`, per-book caches under `books/`, recents list, Wi-Fi credentials). Books are identified by a content-based fingerprint, so cached data survives file moves and renames. See [docs/file-formats.md](./docs/file-formats.md).

---

## Limitations and scope

- **EPUB is the primary target** for advanced DX34 features. TXT and XTC are supported but do not share the full in-book workflow.
- Current releases are intentionally **English-first**.
- The ESP32-C3 has limited RAM, which constrains features like sleep playlist size and the number of files browsable at once.
- **Bluetooth input: work in progress, not available in current release.**

---

## Support and project links

- Ideas: [GitHub Discussions](https://github.com/diogo7dias/crosspoint-reader-DX34/discussions/categories/ideas)
- Bugs: [GitHub Issues](https://github.com/diogo7dias/crosspoint-reader-DX34/issues)
- Governance: [GOVERNANCE.md](./GOVERNANCE.md)
- Tip jar: [Ko-fi](https://ko-fi.com/d7d7m)

---

## Attribution

This project is independent and unaffiliated with Xteink.

DX34 is a fork of **CrossPoint**, originally created by [@daveallie](https://github.com/daveallie) and the CrossPoint team.

- [CrossPoint (upstream)](https://github.com/crosspoint-reader/crosspoint-reader) — original firmware by @daveallie and contributors
- [atomic14/diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader)
