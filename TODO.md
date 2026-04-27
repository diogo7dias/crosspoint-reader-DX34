# TODO

Open follow-ups for this firmware. Prioritised top-down. Workflow per item: build → flash → user tests on device → ship in next release. No soak windows, no waiting periods — this is a hobby project, not a paid product.

## Next release inclusions

Items already merged to `main` that should be called out in the release notes for the next version. Move into the release body when cutting the tag, then clear this section.

### v2.0.2

- **Reader open: half-refresh fast path on same-book / same-settings re-entry** ([#117](https://github.com/diogo7dias/crosspoint-reader-DX34/pull/117), `6376368`). EPUB / XTC / TXT readers no longer pay the ~1 s full-refresh cost when re-entering the same book at the same font, size, orientation, theme, and dither. Cold open, different book, font/orientation/theme change still full-refresh — ghost-prone cases stay covered. Returning from menus / bookmarks / TOC and resuming the last book at boot are visibly faster.

- **Fix: per-book theme no longer leaks into global settings.** In-book toggles (orientation long-press, text render mode, custom-font safety reverts) used to call `SETTINGS.saveToFile()` directly, which wrote the per-book theme overlay (font, size, margins, etc.) back to `/.crosspoint/settings.json` as the new global default. Routed all 8 reader-side save sites through a new `ReadingThemeStore::persistContextual()` that picks per-book or global by context. The "random book on boot" toggle inside the reader menu now snapshots the per-book theme, reloads the on-disk globals, flips the flag, saves clean, and restores the overlay. XTC reader also now loads its per-book `reader_settings.json` on open, for parity with EPUB / TXT. Net effect: global defaults are only ever changed via the global Settings UI, exactly as documented.

## Active

- [ ] **Flip `useFactoryLUT` default to ON.** Today the toggle ships off in [src/CrossPointSettings.h](src/CrossPointSettings.h). Default-on means new users get the sharper sleep-cover rendering immediately. Existing on-SD cover BMPs were generated with the prior dither + double-bright, so they'll render slightly darker until the user runs Library Refresh — call that out in the release notes for the version that flips it.

- [ ] **PXC visibility in MyLibrary file browser.** Today `.pxc` files are only picked up when dropped directly into `/sleep/`. Wiring needed:
  - [MyLibraryActivity.cpp](src/activities/home/MyLibraryActivity.cpp): add `isPxcFile()`, expand `isManagedFile()` to OR it. PXC files should appear in the browser list.
  - Action menu for PXC files: Move-to-Sleep + Delete + Rename. Skip the BMP-only "Open Image" item (no PXC viewer yet).
  - [UITheme.cpp](src/components/UITheme.cpp): map `.pxc` to the existing image icon.

- [ ] **`PxcViewerActivity`** for full-screen PXC viewing from the file browser. Model on the `renderPxcSleepScreen` flow in [SleepActivity.cpp](src/activities/boot_sleep/SleepActivity.cpp). Pairs with the browser-visibility item above.

- [ ] **EPUB factory LUT for image pages.** Today image pages stay on the differential overlay because the upstream full-page approach needs a 48 KB transient `secondaryFrameBuffer` we can't afford on a ~26 KB-free-heap device. Possible paths:
  - Wait for SDK-side support of windowed factory absolute drive (would let us factory-LUT only the image rect, keep BW text untouched).
  - Or improve heap headroom enough that the secondary buffer fits.
  - Either path is a bigger effort than the others on this list.

- [ ] **XTC factory LUT.** Upstream rewrites `lib/Xtc/Xtc.cpp` and adds an `XtcParser`. Big diff. Wait for clearer user demand or a reason to do the parser rewrite for other purposes.

## SDK fork hygiene

- [ ] **Merge `feat/factory-lut` → `main` on `diogo7dias/community-sdk`.** Today the firmware submodule pins commit `9e21cfe` on the `feat/factory-lut` branch; SDK fork's `main` is still at `c0a79fe` (pre-cherry-pick). Build works either way (submodules pin to a SHA, not a branch), but having SDK main canonical with what we depend on is cleaner.

## Carryover from earlier sessions

- [ ] **Issue #44 — Gamebrick BLE Plan B.** Surgical Gamebrick decoder port from [thedrunkpenguin/crosspoint-reader-ble](https://github.com/thedrunkpenguin/crosspoint-reader-ble) (`crosspoint-ble-1.2` branch). Their `BluetoothHIDManager.cpp` has the V2 reverse-engineering (MAC prefix `60:4d:ec`, 5-byte report, counter-freeze at `0x07D0`, `byte[4]=0x07/0x09` D-pad, `byte[3]=0x98±` joystick). Port just the decoder into [src/BleHidManager.cpp](src/BleHidManager.cpp) `onHidReport` behind a name-prefix detect (`"IINE"`). Keep NimBLE 1.4.3 (do NOT upgrade to 2.x — drunkpenguin uses the 2.3.6 API). Do NOT port: their full client lifecycle, DeviceProfiles table, button-injector architecture. No hardware available locally — compile-check + ship to a volunteer tester (NOT @ytsejam1138, last attempt bricked their device).

## Not pursuing (hardware-blocked)

- Nintendo Switch Joy-Con / PS DualSense / default-mode 8BitDo / older Xbox Wireless BLE support — all use Bluetooth Classic (BR/EDR), which the ESP32-C3 radio does not have. Only the original ESP32 supports Classic BT; S3/C3/C6/H2 don't. Not fixable without new hardware. Known-working BLE HID inputs: Free2-M, Kobo remote, MINI_KEYBOARD, Gamebrick (pending Plan B), newer Xbox Wireless BLE firmware, 8BitDo `X+Start` BLE-keyboard mode.
