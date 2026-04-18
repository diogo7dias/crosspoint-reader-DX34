# Architecture Overview

This document describes the high-level architecture of the CrossPoint DX34
firmware for developers who want to understand, modify, or contribute to the
codebase.

## Hardware Platform

- **MCU:** ESP32-C3 (single-core RISC-V, 160 MHz)
- **RAM:** ~380 KB total, ~230 KB usable heap after boot
- **Flash:** 16 MB (dual OTA partitions at 6.5 MB each, 3.5 MB SPIFFS, 64 KB coredump)
- **Display:** 480x800 e-ink (4-level grayscale), driven via SPI
- **Storage:** SD card (SdFat library, FAT32)
- **Input:** 5 physical buttons via ADC-multiplexed GPIO
- **Connectivity:** Wi-Fi (STA + AP modes)
- **Power:** LiPo battery with deep-sleep RTC wakeup

### Memory Constraints

The ~230 KB heap is the primary constraint. Key budget items:

| Consumer              | Size       | Notes                                      |
|-----------------------|------------|--------------------------------------------|
| Display framebuffer   | ~48 KB     | Single-buffer mode (480x800 @ 1bpp x2)     |
| Font cache            | ~20-40 KB  | Decompressed glyph bitmaps                 |
| EPUB rendering        | ~30-60 KB  | DOM tree, reflow buffers, image decode      |
| Web server            | ~12-16 KB  | HTTP + WebSocket + upload/download buffers  |
| FreeRTOS overhead     | ~15 KB     | Tasks, semaphores, stacks                   |
| Free                  | ~60-100 KB | Varies by activity                          |

The build uses `-fno-exceptions` (operator `new` returns `nullptr` on failure)
and `-Os` (size optimization). All dynamic allocations should check for `nullptr`.

## Application Structure

```
src/
  main.cpp                 Entry point, activity manager, power management
  CrossPointSettings.h/cpp Settings model + binary migration
  CrossPointState.h/cpp    Runtime state (current book, sleep playlist)
  JsonSettingsIO.h/cpp     Atomic JSON read/write for settings & state
  SettingsList.h           Setting definitions (enums, ranges, UI metadata)
  fontIds.h                Generated font ID constants

  activities/              UI screens
    Activity.h             Base class with render task + lifecycle
    boot_sleep/            Boot logo + sleep screensaver
    reader/                EPUB, TXT, XTC readers + menus
    home/                  Library browser, recents, search
    network/               Wi-Fi, web server, OTA, Calibre
    settings/              System + reader settings UI
    util/                  Dialogs, keyboards, message screens

  network/                 Backend networking
    CrossPointWebServer.*  HTTP + WebSocket file transfer server
    OtaUpdater.*           GitHub-based OTA firmware updates
    HttpDownloader.*       HTTP/HTTPS client for OPDS + downloads

  components/              UI theming (colors, fonts, spacing)
  images/                  Boot splash bitmaps (compiled in)
  util/                    String helpers, button navigation, drawing utils
```

## Activity System

All UI screens inherit from `Activity` (see `src/activities/Activity.h`).

### Lifecycle

```
Construction -> onEnter() -> [loop() / render()] -> onExit() -> Destruction
```

- **`onEnter()`**: Called once when the activity becomes active. Initialize
  state, start render task.
- **`loop()`**: Called every main-loop tick (~10 ms). Handle input, update state.
- **`render(RenderLock&&)`**: Called on a dedicated FreeRTOS task when
  `requestUpdate()` is invoked. Draw to the framebuffer.
- **`onExit()`**: Called before destruction. Save state, stop render task.

### Render Task

Each activity spawns a FreeRTOS task for rendering. This prevents long reflow/
drawing operations from blocking button input. The `RenderLock` RAII guard
ensures the activity is not deleted mid-render.

### Activity Transitions

`main.cpp` holds a global `currentActivity` pointer. Activities trigger
transitions via callbacks (e.g., `onGoHome()`, `onGoToReader(path)`). The
callback calls `exitActivity()` (which invokes `onExit()` + delete) then
constructs the new activity and calls `onEnter()`.

**Critical pattern:** Lambda callbacks must not reference captured variables
after `exitActivity()` — the enclosing activity object has been deleted.

## Settings System

Settings are persisted as JSON at `/.crosspoint/settings.json` on the SD card.

### Write Path (Atomic)

`JsonSettingsIO::safeWriteFile()` uses a write-rename pattern to prevent
corruption on power loss:

1. Write to `settings.json.tmp`
2. Rename `settings.json` to `settings.json.bak`
3. Rename `settings.json.tmp` to `settings.json`

If step 1 fails (e.g., stale `.tmp` from a previous crash), falls back to
`.tmp2` as the temporary name.

### Read Path (With Fallback)

`safeReadFile()` reads `settings.json` first; if empty/missing, falls back to
`settings.json.bak`.

### Migration

Old binary-format settings (pre-DX34) are loaded via `loadFromBinaryFile()`
and immediately re-saved as JSON. Legacy enum values (e.g., `LEGACY_GEORGIA`)
are normalized to current values by `normalizeFontFamily()`.

## Network Subsystem

### Web Server (`CrossPointWebServer`)

Runs on the main loop (no separate task). Provides:

- **HTTP endpoints**: File list, upload (multipart POST), download, settings
  page, CRUD operations (rename, move, delete, create folder).
- **WebSocket**: Binary chunked file upload/download for faster transfers.
- **UDP discovery**: Broadcasts on ports 54982+ so the web UI can auto-detect
  the device on the local network.

The web UI HTML/JS is compiled into firmware via `scripts/build_html.py`.

### OTA Updates (`OtaUpdater`)

Checks the GitHub Releases API for newer firmware, downloads the binary via
ESP-IDF's HTTPS OTA, and writes to the secondary OTA partition. On success,
reboots to the new firmware; on failure, the old partition remains active.

## Reader Pipeline

### EPUB

1. **Parse**: `lib/Epub/` uses expat (XML parser) to extract the OPF manifest,
   spine, and NCX/nav table of contents.
2. **Reflow**: HTML content is parsed and reflowed to the display width with
   the configured font, margins, and line spacing. CSS is partially supported.
3. **Render**: Reflowed text and inline images are drawn to the framebuffer via
   `GfxRenderer`.
4. **Paginate**: Reflow output is split into pages based on the display height.

### TXT

Loaded in UTF-8 chunks, word-wrapped to the display width, and paginated.

### XTC

Pre-rendered bitmap pages (1-bit or 2-bit). No reflow — pages are displayed
directly. Much lower memory usage than EPUB.

## Font System

Fonts are stored compressed in flash. On first use, glyphs are decompressed
into a RAM cache (`FontCacheManager`). The cache evicts least-recently-used
glyphs when full.

Font families: ChareInk (primary), Bookerly, Vollkorn, Atkinson Hyperlegible (13/16 pt only).
Sizes: Small through X-Large (mapped to point sizes per family).

**Note:** Unifont must remain in the build even if it appears unused — removing
it breaks UI font size calculations (see MEMORY.md).

## Build Environments

| Environment    | Serial | Log Level | Purpose                    |
|----------------|--------|-----------|----------------------------|
| `default`      | Off    | None      | Production release         |
| `debug`        | On     | 2 (DBG)   | Development diagnostics    |
| `gh_release`   | Off    | 0 (ERR)   | GitHub release builds      |
| `gh_release_rc`| Off    | 1 (INF)   | Release candidates         |
| `slim`         | Off    | None      | Space-optimized build      |

Serial output is only initialized if USB is physically connected
(`gpio.isUsbConnected()`).

## Power Management

The main loop tracks idle time. After the configured timeout (default 10 min),
the device enters deep sleep:

1. Save state (current book, page, sleep playlist position).
2. Render sleep image (if configured).
3. Wait for display refresh to complete.
4. Enter RTC deep sleep with power-button wakeup.

On wake, `setup()` checks the wakeup reason and either resumes the last book
or returns to the home screen.

## Crash Recovery

The build wraps `panic_print_backtrace` and `panic_abort` (via linker
`--wrap`) to capture crash data to RTC memory, which survives reboots.
The 64 KB coredump partition provides additional post-mortem data.

## Directory Guide

| Path                    | Contents                                    |
|-------------------------|---------------------------------------------|
| `src/`                  | Application code (activities, network, util)|
| `lib/`                  | Project libraries (Epub, fonts, renderers)  |
| `open-x4-sdk/`          | Hardware drivers (git submodule)            |
| `scripts/`              | Build scripts (HTML gen, i18n, image patch) |
| `test/`                 | Test EPUBs, hyphenation evaluation          |
| `docs/`                 | Design docs, API reference, release notes   |
| `partitions.csv`        | Flash partition layout                      |
| `platformio.ini`        | Build configuration                         |
