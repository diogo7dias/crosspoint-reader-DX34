# CrossPoint-Mod-DX34

Firmware for the **Xteink X4 / DX34-class ESP32-C3 e-paper reader**, maintained as a **DX34-focused fork** of CrossPoint.

This repository is not a mirror of upstream CrossPoint. It is a productized fork with its own UX choices, release cadence, defaults, and device priorities.

![](./docs/images/cover.jpg)

---

## What This Firmware Is

`CrossPoint-Mod-DX34` is a custom firmware build for people who want the X4/DX34 hardware to behave like a practical reading device first:

- faster access to books
- stronger EPUB reading controls
- cleaner DX34-specific UI behavior
- configurable sleep wallpapers
- Wi-Fi transfer and OTA updates
- reader customization without leaving the book

This fork keeps upstream credit and history, but it does **not** aim for strict parity with upstream behavior.

---

## What's Different From Mainline CrossPoint

This is an independent fork. DX34 behavior and stability are always prioritized over upstream parity. Here's what sets it apart:

| Area | DX34 Mod | Upstream CrossPoint |
|------|----------|---------------------|
| **UX language** | English-first | Multi-language |
| **Font families** | ChareInk, Bookerly, Vollkorn, IM Fell DW Pica | Different font set |
| **In-book workflow** | Reading Themes, live settings, highlights, footnotes | Basic reader controls |
| **Sleep wallpapers** | Managed folder system with favorites, trimming, randomization | Basic sleep image |
| **Reader settings** | Extensive (word spacing, indent modes, render modes, etc.) | Fewer options |
| **Release cadence** | Independent DX34 releases | Separate schedule |
| **Home screen** | DX34-specific layout with recents and version label | Generic layout |

More fork positioning notes live in [docs/differences-from-upstream.md](./docs/differences-from-upstream.md).

---

## Installation

### Before you flash: back up your SD card

If you are switching from another firmware (original CrossPoint, stock Xteink, or any other build), **back up the entire contents of your SD card to a folder on your computer first**.

**Why this matters:**
- This firmware creates its own data folder (`/.crosspoint/`) on the SD card at first boot — that part is automatic.
- However, **your reading progress from the previous firmware will not carry over**. The DX34 mod stores book positions in its own format under `/.crosspoint/books/`. If the previous firmware used a different format or location, those saved positions are lost.
- Your **book files are safe** — the firmware does not delete or modify your EPUBs, TXTs, or other content files. But having a full backup means you can always go back if needed.

**How to back up:**
1. Turn off the device.
2. Remove the micro SD card.
3. Insert it into your computer (use an adapter if needed).
4. Copy **everything** from the SD card into a folder on your PC (e.g., `X4-SD-Backup`).
5. Re-insert the SD card into the device.

Once backed up, you can safely flash.

### Web flasher (easiest)

1. Connect the device by USB-C.
2. Wake or reboot it if needed.
3. Open [xteink.dve.al](https://xteink.dve.al/).
4. Flash either the latest firmware or a downloaded `.bin`.

### Releases

Release binaries are published here:

- [DX34 releases](https://github.com/diogo7dias/crosspoint-reader-DX34/releases)

### Manual flashing with PlatformIO

```sh
pio run -t upload
```

---

## Features at a Glance

- [Supported book formats](#supported-book-formats) — EPUB, TXT/MD, XTC/XTCH
- [EPUB reading experience](#epub-reading-experience) — in-book menu, reading themes, highlights, footnotes
- [Fonts and sizes](#fonts-and-sizes) — 4 font families; ChareInk/Bookerly/Vollkorn at 12/14/16/17 pt, IM Fell DW Pica at 15 pt
- [Reader layout and appearance](#reader-layout-and-appearance) — full control over text rendering
- [Sleep screen and wallpaper system](#sleep-screen-and-wallpaper-system) — managed wallpaper folder with favorites
- [Home screen and library](#home-screen-and-library) — recents, file browser, search, QR sharing
- [Wireless features](#wireless-features) — Wi-Fi transfer, OPDS, Calibre, KOReader sync, OTA
- [Device and system controls](#device-and-system-controls) — buttons, sleep, refresh, orientation, random book on boot

---

## Supported Book Formats

| Format | Description | Theme Support |
|--------|-------------|---------------|
| **EPUB** | Primary reading format (EPUB 2 and EPUB 3) — all advanced DX34 features live here | Yes |
| **TXT / MD** | Plain-text reader with progress tracking | No |
| **XTC / XTCH** | Pre-rendered page format with chapter selection | No |

EPUB is where the advanced DX34 reading work lives. The EPUB reader supports inline images (JPEG and PNG), partial CSS styling, and HTML entity decoding. TXT and XTC are supported but they do not share the full EPUB in-book settings and theme workflow.

---

## EPUB Reading Experience

The EPUB reader is the core of the DX34 firmware. It includes persistent reading position, chapter navigation, configurable text rendering, and a rich in-book workflow.

| Crisp mode | Dark mode | Bionic mode |
|:---:|:---:|:---:|
| <img src="./docs/images/screenshots/reader-crisp.jpg" width="250"/> | <img src="./docs/images/screenshots/reader-dark.jpg" width="250"/> | <img src="./docs/images/screenshots/reader-bionic.jpg" width="250"/> |

### In-book menu

Pressing **OK** while reading an EPUB opens the in-book menu:

| Menu Item | What It Does |
|-----------|--------------|
| Select Chapter | Jump to any chapter in the book |
| Footnotes | Jump to the footnote index (only shown for books with footnotes/endnotes) |
| Reading Orientation | Switch between portrait and landscape modes |
| Reading Themes | Open the theme manager (save, apply, rename, delete themes) |
| Sync Progress | Sync reading position with a KOReader server |
| Clean Cache + Progress | Clear cached data and reading position for this book |
| Delete Book | Remove the book file from the device |
| Remove from Recents | Remove the book from the recent books list |
| **Wallpaper Triage** | *(only shown when a last sleep wallpaper exists)* |
| Favorite / Unfavorite | Mark/unmark the last sleep wallpaper as a favorite |
| Pause / Unpause Rotation | Pause or resume wallpaper rotation |
| Move to sleep pause | Move the last wallpaper out of the active rotation |
| Delete Wallpaper | Delete the last sleep wallpaper |
| **Extras** | |
| Random Book on Boot | Toggle opening a random book from recents on device boot |

| In-book menu | Chapter selection |
|:---:|:---:|
| ![In-book menu](./docs/images/screenshots/in-book-menu.jpg) | ![Chapter selection](./docs/images/screenshots/chapter-selection.jpg) |

### Reading Themes

The EPUB-only **Reading Themes** system lets you save and switch between different display configurations:

**How it works:**

1. **Adjust settings live** — change font, size, margins, spacing, or any reader setting while inside a book. Changes apply immediately without saving a theme.

2. **Save as a theme** — capture the current display setup under a name for later use.

3. **Manage themes** — up to 16 named themes per book:
   - Apply a saved theme
   - Rename a theme
   - Overwrite a theme with current settings
   - Delete a theme

When you apply a theme, the firmware reflows the current chapter and restores your reading position approximately, so you stay near the same spot after font or margin changes.

![Reading Themes with theme actions popup](./docs/images/screenshots/reading-themes.jpg)

### Highlights and Quotes

The firmware includes a text selection system for saving passages from EPUBs.

**How to activate:** **Long press OK (hold for 1 second)** while reading. A cursor appears on the first word of the current page.

**How it works (step by step):**

1. **Enter highlight mode** — long press OK. A cursor appears on the first word of the current page.

2. **Pick the start word** — move the cursor to the first word of the passage you want to save:
   - **Left / Right** (or Page buttons): move one word at a time
   - **Up / Down** (side buttons): jump one line up or down
   - **OK**: confirm the start word

3. **Pick the end word** — after confirming the start, the cursor jumps to the end of the page. Move it to the last word of your passage:
   - Same navigation controls as step 2
   - The end cursor can cross onto the next page if your passage spans multiple pages
   - It won't go before the start word
   - **OK**: confirm the end word

4. **Review and save** — the selected text is shown underlined for 3 seconds, then automatically saved. A "Quote saved!" confirmation appears.

5. **Cancel at any time** — press **Back** to exit highlight mode without saving.

**Where quotes are stored:**

Quotes are saved as `_QUOTES.txt` sidecar files next to the book file. For example, a book at `/recents/MyBook.epub` produces `/recents/MyBook_QUOTES.txt`.

Each quote entry includes the chapter title and the selected text:

```
[Chapter 3: The Journey]
The selected passage text goes here.
---
```

Quote files are preserved when you remove a book from recents or delete it. You can open the `_QUOTES.txt` file on the device to review your saved passages.

![Quotes file viewed on device](./docs/images/screenshots/quotes-file.jpg)

| Normal reading | Selection active | Quote saved |
|:---:|:---:|:---:|
| ![Normal](./docs/images/screenshots/highlight-normal.jpg) | ![Selection](./docs/images/screenshots/highlight-selection.jpg) | ![Saved](./docs/images/screenshots/highlight-saved.jpg) |

### Footnote Navigation

When reading EPUBs with footnotes or endnotes, tapping a footnote link jumps to the note. The firmware maintains a position stack up to 3 levels deep, so you can follow nested references and return to your original reading position.

### Reader Shortcuts

While reading an EPUB:

| Action | What It Does |
|--------|--------------|
| **Single tap OK** | Open the in-book menu |
| **Double tap OK** | Cycle text render mode (Crisp / Dark / Bionic) |
| **Long press OK (1 second)** | Enter highlight / quote selection mode |
| **Back** | Return home (or return from footnote) |
| **Side / front page buttons** | Previous / next page |

---

## Fonts and Sizes

The DX34 firmware ships with **four built-in reader font families**, each with full **regular**, **bold**, and **italic** styles.

### Font family overview

| Font | Style | Sizes Available | Character |
|------|-------|-----------------|-----------|
| **ChareInk** | Serif, optimized for e-ink | 12, 14, 16, 17 | DX34-specific, clean and readable |
| **Bookerly** | Serif | 12, 14, 16, 17 | Amazon's reading font |
| **Vollkorn** | Serif | 12, 14, 16, 17 | Open-source book font |
| **IM Fell DW Pica** | Antique serif, historical-look | 15 | Period-authentic 17th-century revival; Bold rendered via synthetic emboldening |

| ChareInk | Bookerly | Vollkorn |
|:---:|:---:|:---:|
| ![ChareInk](./docs/images/screenshots/reader-crisp.jpg) | ![Bookerly](./docs/images/screenshots/font-vollkorn.jpg) | ![Vollkorn](./docs/images/screenshots/highlight-normal.jpg) |

### Size details

ChareInk, Bookerly, and Vollkorn share a 4-size set:

| Size | Point Size |
|------|-----------|
| 1 | 12 pt |
| 2 | 14 pt |
| 3 | 16 pt (default) |
| 4 | 17 pt |

IM Fell DW Pica ships at one size only: **15 pt**.

### How size switching works

- ChareInk, Bookerly, and Vollkorn use the same size set, so switching between them preserves the exact size.
- Switching to IM Fell forces the size to 15 pt (its only supported size); switching away restores a size from the shared set.
- Settings saved under older firmware are migrated automatically to the nearest current size (10/13→12, 18/19→17 for non-IMFell families).

### Additional font notes

- ChareInk, Bookerly, and Vollkorn support **regular**, **bold**, and **italic** rendering (ChareInk and Bookerly also support bold italic). IM Fell supports **regular** and **italic**; its **bold** is synthesized at render time.
- Default line spacing is **90%** for all families.
- UI text (menus, popups, status bar) uses separate built-in bitmap fonts, not the reader font.
- The firmware includes **Unifont** internally as a Unicode fallback for broader glyph coverage, but it is not selectable as a reader font.

---

## Reader Layout and Appearance

The firmware exposes extensive control over how text is rendered. All of these can be changed live while reading an EPUB.

### Text settings

| Setting | Options | Default |
|---------|---------|---------|
| **Font family** | ChareInk, Bookerly, Vollkorn, IM Fell DW Pica | ChareInk |
| **Font size** | 12, 14, 16, 17 pt (ChareInk/Bookerly/Vollkorn); 15 pt (IM Fell) | 16 pt |
| **Line spacing** | 65% to 150% | 90% |
| **Paragraph alignment** | Justified, Left, Center, Right, Book Style | Justified |
| **First-line indent** | Book (follow CSS), Off, Small, Medium, Large | Book |
| **Word spacing** | -30%, 0% (Normal), +80%, +150%, +240% | 0% (Normal) |
| **Extra paragraph spacing** | Off, Small, Medium, Large | Off |
| **Text render mode** | Crisp, Dark, Bionic (quick cycle: double tap OK) | Crisp |
| **Reader style mode** | User (your settings override), Hybrid (blend with book CSS) | User |
| **Embedded CSS** | On / Off | On |
| **Hyphenation** | On / Off | — |
| **Bold swap** | On / Off | Off |
| **Debug borders** | On / Off | Off |

| Device settings | In-book reader settings |
|:---:|:---:|
| ![Settings screen](./docs/images/screenshots/settings-display-reader.jpg) | ![Reader settings](./docs/images/screenshots/reader-settings.jpg) |

### Margins

| Setting | Range | Default |
|---------|-------|---------|
| **Horizontal margin** | 0–55 px | — |
| **Top margin** | 0–55 px | — |
| **Bottom margin** | 0–55 px | — |
| **Uniform margins** | Toggle to link all margins | — |

### Orientation

Four orientation modes:

- **Portrait** (default)
- **Landscape CW** (clockwise)
- **Inverted** (upside down)
- **Landscape CCW** (counter-clockwise)

### Status bar

The status bar is highly configurable. Each element can be independently shown/hidden and positioned.

| Element | Options |
|---------|---------|
| **Status bar** | Show / Hide |
| **Battery** | Show / Hide, position (6 positions: top/bottom + left/center/right) |
| **Page counter** | Show / Hide, mode (Current/Total or Left in Chapter), position |
| **Book percentage** | Show / Hide, position (6 positions: top/bottom + left/center/right) |
| **Chapter percentage** | Show / Hide, position |
| **Book progress bar** | Show / Hide, position, style (Thin / Thick / Dotted) |
| **Chapter progress bar** | Show / Hide, position, style |
| **Chapter title** | Show / Hide, position, truncation control |
| **Font size** | Small or Medium |
| **Bar thickness** | Normal or Double |
| **Text alignment** | Right, Center, Left |
| **Hide battery %** | Never, In Reader, Always |

| Status bar — title top, pages left bottom | Status bar — percentages top, title bottom |
|:---:|:---:|
| ![Status bar config 1](./docs/images/screenshots/statusbar-config1.jpg) | ![Status bar config 2](./docs/images/screenshots/statusbar-config2.jpg) |

---

## Sleep Screen and Wallpaper System

This firmware has a more developed sleep wallpaper system than upstream.

### Sleep modes

| Mode | Behavior |
|------|----------|
| **Dark** | Solid dark screen |
| **Light** | Solid light screen |
| **Custom** | Show a wallpaper from the `/sleep` folder |
| **Cover** | Show the cover of the last opened book |
| **None (Blank)** | Leave the screen as-is |
| **Cover + Custom** | Show the book cover if available, otherwise a custom wallpaper |

### Cover display options

When using Cover or Cover + Custom mode:

| Setting | Options |
|---------|---------|
| **Cover mode** | Fit (letterbox) or Crop (fill screen) |
| **Cover filter** | None, Black & White, Inverted Black & White |

### Custom wallpaper locations

The firmware looks for custom BMP sleep wallpapers in this order:

1. `/sleep/` folder images
2. `/sleep_F.bmp`
3. `/sleep.bmp`

If no valid custom bitmap is found, it falls back to the default DX34 sleep screen.

### Random wallpaper selection

The behavior depends on how many BMPs are in `/sleep`:

**Up to 200 images:**
- The firmware keeps a persisted playlist in memory and on SD.
- By default, it follows a stable file-based order.
- Using **Randomize Sleep Images** in settings reshuffles the playlist.
- New images added to `/sleep` are pushed near the front so they appear quickly.

**More than 200 images:**
- The firmware avoids storing the full playlist in memory (ESP32-C3 has limited RAM).
- **Randomize Sleep Images** chooses a random starting wallpaper.
- After that, the device advances sequentially through the sorted filenames.

### Wallpaper protection and trimming

The sleep folder has a practical cap of **200 persisted entries**.

If `/sleep` grows beyond that:
- Favorite wallpapers are protected first.
- Non-favorites beyond the limit are automatically moved to `/sleep pause`.

This prevents the wallpaper system from growing until it becomes unstable or too memory-heavy.

### Favorites

Sleep wallpapers can be marked as favorites:

- Favorite files are stored with an `_F` suffix.
- Favorites display with a `[F]` prefix in UI labels.
- Favorites inside `/sleep` are protected from automatic trimming.
- The protected favorites count is limited to **200**.

### Wallpaper management tools

The Settings UI includes tools for managing sleep wallpapers:

- Randomize sleep images
- Inspect the last sleep wallpaper
- Pause / unpause wallpaper rotation
- Move the last wallpaper to `/sleep pause`
- Favorite / unfavorite a wallpaper
- Delete a wallpaper
- Show the wallpaper filename on-screen (toggle)

| Status bar settings | Controls & system settings |
|:---:|:---:|
| ![Status bar settings](./docs/images/screenshots/settings-statusbar.jpg) | ![Controls and system settings](./docs/images/screenshots/settings-controls-system.jpg) |

---

## Home Screen and Library

### Home screen

The home screen is DX34-specific and includes:

- DX34 version label and header
- **Recent books** list with progress percentage (up to 100 books)
- Menu entries: My Library, Recents, File Transfer, Settings
- Optional OPDS browser entry (when an OPDS server is configured)
- Sleep favorites capacity warning when the protected wallpaper list is full
- **Home layout** setting: **Classic** (list of recent books) or **Single Cover** (large cover art display)

![Home screen](./docs/images/screenshots/home-screen.jpg)

### Recent books

- Up to 100 recent books tracked.
- Books are moved to the `/recents/` folder when opened (keeping them organized).
- Each book shows its reading progress percentage.
- Books can be removed from recents via the home screen or the in-book menu.
- Quote files (`_QUOTES.txt`) are preserved when removing or deleting books.

### My Library (file browser)

The built-in file browser lets you manage files on the SD card:

| Feature | Description |
|---------|-------------|
| **Browse folders** | Navigate the SD card directory tree |
| **Open books** | Open EPUB, TXT, MD, XTC, XTCH files |
| **Search** | Real-time fuzzy search across file names |
| **Move files** | Move files to a different folder with destination picker |
| **Rename files** | Rename files with on-device keyboard |
| **Delete files** | Delete files with confirmation |
| **View BMPs** | Preview BMP images directly on the e-ink screen |
| **Show hidden files** | Toggle visibility of files and directories starting with `.` |
| **Load more** | Pagination for large folders (200 files at a time, 1000 for `/sleep`) |

### QR code file sharing

You can share individual files from the device over Wi-Fi:
1. Select a file in the browser.
2. Choose the QR share option.
3. A QR code is displayed on screen with a download link.
4. Scan the QR code from your phone to download the file.

Requires an active Wi-Fi connection.

---

## Wireless Features

### Built-in web server

The Wi-Fi file-transfer mode starts a web server accessible from any browser on the same network.

| Feature | Description |
|---------|-------------|
| **Upload** | Upload files from phone or desktop (with resume support) |
| **Browse** | Browse the SD card directory tree |
| **Create folders** | Make new directories |
| **Move / Rename / Delete** | Full file management |
| **Search** | Fuzzy search for books |
| **Settings page** | View and change device settings from the browser |
| **Status page** | Firmware version, IP address, free space |

The web server supports both STA mode (join existing network) and AP mode (device creates its own hotspot). A WebSocket server (port 81) handles binary chunked transfers for faster upload and download speeds. UDP auto-discovery broadcasts let the web UI detect the device on the network automatically.

See [docs/webserver.md](./docs/webserver.md) and [docs/webserver-endpoints.md](./docs/webserver-endpoints.md) for details.

<!-- screenshot: web-transfer -->

### Calibre wireless transfer

The device starts a web server optimized for Calibre wireless transfers and displays connection instructions on screen.

### OPDS browser

Configure an OPDS server URL, username, and password in settings, then browse and download books directly from the device.

### KOReader progress sync

Sync your reading position across devices using a KOReader-compatible server:

- Stored credentials (server URL, username, password)
- Configurable document matching method: by filename or by binary content hash
- In-book sync action: compare local and remote progress, then apply or upload

### OTA updates

The firmware can check for and install updates over Wi-Fi directly from GitHub releases. Version comparison prevents accidental downgrades. The device uses dual OTA partitions, so a failed update can safely roll back to the previous firmware.

---

## Device and System Controls

### Button controls

| Setting | Options |
|---------|---------|
| **Side button layout** | Previous/Next or Next/Previous |
| **Long-press chapter skip** | Toggle long-press on side buttons to skip chapters |
| **Short power button action** | Ignore, Sleep, Page Turn, or Refresh Screen |
| **Front button remapping** | Remap Back, Confirm, Left, Right to any physical front button |

Front button remapping uses an interactive step-by-step flow where you press the physical button you want for each logical role.

### Display and power

| Setting | Options |
|---------|---------|
| **Auto-sleep timeout** | 1, 5, 10, 15, or 30 minutes |
| **E-ink refresh frequency** | Full refresh every 1, 5, 10, 15, or 30 pages |
| **Sunlight fading compensation** | On / Off |
| **Custom boot image** | Place `/boot.bmp` on the SD card |
| **Show hidden files** | Show files/folders starting with `.` in the file browser |
| **Random book on boot** | Open a random book from recents instead of the last book |

### Orientation

The display orientation can be set globally or toggled from the in-book menu:

- Portrait (default)
- Landscape CW
- Inverted
- Landscape CCW

---

## Storage, Cache, and On-SD State

The firmware stores runtime state and caches under `/.crosspoint/` on the SD card.

This includes:
- `settings.json` — all user preferences
- `state.json` — runtime state (sleep playlist, last book, etc.)
- `books/` — per-book caches (reading position, page index, themes)
- Recent books list
- Wi-Fi credentials

The device aggressively caches parsed content to reduce RAM pressure and make repeated opens faster. Books are identified by a content-based fingerprint, so cached data (reading position, page index, themes) survives file moves and renames.

Useful internals docs:
- [docs/file-formats.md](./docs/file-formats.md)
- [docs/webserver-endpoints.md](./docs/webserver-endpoints.md)
- [docs/release-checklist.md](./docs/release-checklist.md)

---

## Development

### Prerequisites

- PlatformIO Core (`pio`) or VS Code + PlatformIO
- Python 3.8+
- USB-C cable
- Xteink X4 / DX34-compatible device

### Checkout

```sh
git clone --recursive https://github.com/diogo7dias/crosspoint-reader-DX34
cd crosspoint-reader-DX34
git submodule update --init --recursive
```

### Build

```sh
pio run
```

### Flash

```sh
pio run -t upload
```

### Serial monitor / debugging

```sh
pio device monitor
```

Or use the helper:

```sh
python3 scripts/debugging_monitor.py
```

macOS example:

```sh
python3 scripts/debugging_monitor.py /dev/cu.usbmodem101
```

### Host-side tests

Pure-logic tests that run on your laptop (no ESP32 hardware needed) live
under `test/test_*/` and use PlatformIO's Unity framework.

```sh
pio test -e test_host
```

The `test_host` environment uses `platform = native`. Currently covered:

- `test/test_activity_router/` — `lifecycle::ActivityRouter` policy, pending
  coalesce, deep-sleep sequencing, and make* synthesizers

Sources compiled into this env are explicitly whitelisted in
`platformio.ini` via `build_src_filter` — Arduino/FreeRTOS-dependent code
is excluded. When adding a new module that needs host tests, either keep
it free of Arduino includes or provide a `#ifdef UNIT_TEST_HOST` stub
header (see `src/lifecycle/ActivityStubForHostTest.h`).

CI integration (GitHub Actions) is not yet wired up — tracked as follow-up.

---

## Limitations and Scope Notes

- **EPUB is the primary target** for advanced DX34 features. TXT and XTC are supported but do not share the full in-book settings/theme workflow.
- Current releases are intentionally **English-first**.
- Hyphenation exists in the codebase but is not positioned as a flagship DX34 feature.
- The ESP32-C3 has limited RAM (~182 KB free heap), which constrains features like sleep playlist size and the number of files that can be browsed at once.

---

## Support and Project Links

- Ideas: [GitHub Discussions](https://github.com/diogo7dias/crosspoint-reader-DX34/discussions/categories/ideas)
- Bugs: [GitHub Issues](https://github.com/diogo7dias/crosspoint-reader-DX34/issues)
- Governance: [GOVERNANCE.md](./GOVERNANCE.md)
- Tip jar: [Ko-fi](https://ko-fi.com/d7d7m) — if you enjoy the firmware and want to support development

---

## Attribution

This project is independent and unaffiliated with Xteink.

DX34 is a fork of **CrossPoint**, originally created by [@daveallie](https://github.com/daveallie) and the CrossPoint team. Their work laid the foundation that this firmware builds on.

- [CrossPoint (upstream)](https://github.com/crosspoint-reader/crosspoint-reader) — original firmware by @daveallie and contributors
- [atomic14/diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader)
