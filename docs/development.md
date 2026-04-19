# Development

## Prerequisites

- PlatformIO Core (`pio`) or VS Code + PlatformIO
- Python 3.8+
- USB-C cable
- Xteink X4 / DX34-compatible device

## Checkout

```sh
git clone --recursive https://github.com/diogo7dias/crosspoint-reader-DX34
cd crosspoint-reader-DX34
git submodule update --init --recursive
```

## Build

```sh
pio run
```

Two environments are available:

- `default` — production build, serial logging disabled
- `debug` — full logging (`ENABLE_SERIAL_LOG`, `LOG_LEVEL=2`)

## Flash

```sh
pio run -t upload
```

Use `--upload-port` to target a specific port, e.g. `/dev/cu.usbmodem101` on macOS.

## Serial monitor

```sh
pio device monitor
```

Or use the helper script, which adds reset-and-capture:

```sh
python3 scripts/debugging_monitor.py /dev/cu.usbmodem101
```

## Host-side tests

Pure-logic tests that run on your laptop (no ESP32 hardware needed) live under `test/test_*/` and use PlatformIO's Unity framework.

```sh
pio test -e test_host
```

The `test_host` environment uses `platform = native`. Sources compiled into this environment are explicitly whitelisted in `platformio.ini` via `build_src_filter` — Arduino and FreeRTOS-dependent code is excluded.

When adding a module that needs host tests, either keep it free of Arduino includes or provide a `#ifdef UNIT_TEST_HOST` stub header (see `src/lifecycle/ActivityStubForHostTest.h`).

GitHub Actions CI integration is not yet wired up — tracked as follow-up.
