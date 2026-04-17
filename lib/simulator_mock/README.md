# CrossPoint Simulator

A desktop simulator for [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader)-based firmware. Compiles the firmware natively and renders the e-ink display in an SDL2 window — no device required.

> **Platform support:** Currently configured for **macOS (Apple Silicon)** only. The `[env:simulator]` block in your `platformio.ini` contains `-arch arm64` and Homebrew paths (`/opt/homebrew`). Intel Mac users should remove `-arch arm64` and change those paths to `/usr/local`. Linux requires the same path changes plus a replacement for `MD5Builder.h` (which uses the macOS-only `CommonCrypto` API). Native Windows is not supported; use WSL and follow the Linux instructions.

## Prerequisites

SDL2 must be installed on the host machine.

```bash
# macOS
brew install sdl2


# Linux (Debian/Ubuntu)
sudo apt install libsdl2-dev
```

## Integration

Add the `run_simulator.py` script to `./scripts` in the Firmware's project root. This will add a Platformio task in the IDE to quickly run the simulator.

Add the simulator to your firmware's platformio.ini as a lib_dep and configure the [env:simulator] environment:

```ini
[env:simulator]
platform = native
build_flags =
  -std=gnu++2a
  -arch arm64
  -I/opt/homebrew/include
  -L/opt/homebrew/lib
  -lSDL2
  -DSIMULATOR
  -Wno-c++11-narrowing
  -Wno-bidi-chars
  -DCROSSPOINT_VERSION=\"dev-simulator\"
  -DENABLE_SERIAL_LOG
  -DLOG_LEVEL=2
  -DEINK_DISPLAY_SINGLE_BUFFER_MODE=1
  -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES=1
  -DXML_GE=0
  -DXML_CONTEXT_BYTES=1024
  -DUSE_UTF8_LONG_NAMES=1
  -DPNG_MAX_BUFFERED_PIXELS=16416
  -DDISABLE_FS_H_WARNING=1
  -DDESTRUCTOR_CLOSES_FILE=1
lib_ignore = hal, PNGdec, JPEGDEC
extra_scripts =
  pre:scripts/gen_i18n.py
  pre:scripts/git_branch.py
  pre:scripts/build_html.py
  scripts/run_simulator.py
lib_deps =
  simulator=https://github.com/your-org/crosspoint-simulator.git#v1.0.0
  bblanchon/ArduinoJson @ 7.4.2
  ricmoo/QRCode @ ^0.0.1
  links2004/WebSockets @ 2.7.3
```

For local development, replace the git reference with a symlink after you've cloned the repository:

```ini
simulator=symlink://../crosspoint-simulator
```

## Setup

Place EPUB books at `./fs_/books/` relative to the project root. This maps to the `/books/` path on the physical SD card.

## Build and run

```bash
pio run -e simulator
.pio/build/simulator/program
```

Or use the custom PlatformIO target to build and run in one step:

```bash
pio run -e simulator -t run_simulator
```

## Controls

| Key    | Action                             |
| ------ | ---------------------------------- |
| ↑ / ↓  | Page back / forward (side buttons) |
| ← / →  | Left / right front buttons         |
| Return | Confirm / Select                   |
| Escape | Back                               |
| P      | Power                              |

## Notes

**Cache**: On first open of an ebook, an "Indexing..." popup will appear while the section cache is built. If you see rendering issues after a code change that affects layout, delete `./fs_/.crosspoint/` to clear stale caches.

**HAL compatibility:** The simulator implements the HAL interface defined in `lib/hal/*.h`. If your firmware adds a new method to a HAL class and calls it, the simulator will fail to link until a matching stub is added to the relevant Hal\*.cpp here. For most cases this is a one-liner no-op. Open a PR if the change is applicable to all CrossPoint-based forks.
