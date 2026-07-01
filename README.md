# Lector

> ⚠️ **WORK IN PROGRESS.** Lector is an in-development fork — expect bugs and rough edges. It is **not** a stable daily-driver yet. Back up your SD card and use at your own risk.

An EPUB-only custom firmware for the **Xteink X4 / DX34-class ESP32-C3 e-paper reader**, forked from CrossPoint-Mod-DX34 and restoring the compact Cozette UI.

## What this is

A lean, reading-first firmware focused on EPUBs:

- **Five in-flash reader fonts** — Bookerly, Georgia, Helvetica, Verdana, Merriweather (sizes 11–16, regular / bold / italic). All in flash; no SD-card fonts.
- **Crisp and Dark** render modes.
- **Live appearance preview** while you change reader settings.
- **Per-book reading themes**, highlights, bookmarks, and footnotes.
- **Sleep wallpapers** — a managed `/sleep` folder with favorites and in-book triage.
- **Browser Wi-Fi transfer + firmware update** — upload books and update the firmware from the device's own `/update` web page over your network. **No OTA.**

## Install

> Back up your SD card before flashing. Books are not modified, but reading progress from other firmwares is not migrated.

1. Connect the device by USB-C and wake it.
2. Flash `firmware.bin` from the latest [release](https://github.com/diogo7dias/lector/releases), or update from the device's Wi-Fi `/update` page.

Build from source: `pio run -t upload`.

## Scope

- EPUB is the target. TXT is supported but basic (no in-book themes or settings).
- English-first.
- Deliberately lean: no OTA, OPDS, or Calibre-Web.
- GPIO-only — no Bluetooth/BLE input.

## Attribution

Independent and unaffiliated with Xteink. Lector is a fork of DX34, itself a fork of **CrossPoint** by [@daveallie](https://github.com/daveallie) and the CrossPoint team.

- [CrossPoint (upstream)](https://github.com/crosspoint-reader/crosspoint-reader)
- [atomic14/diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader)
