# TODO

Open follow-ups for this firmware. Prioritised top-down. Workflow per item: build → flash → user tests on device → ship in next release. No soak windows, no waiting periods — this is a hobby project, not a paid product.

## Next release inclusions

Items already merged to `main` that should be called out in the release notes for the next version. Move into the release body when cutting the tag, then clear this section.

### v2.0.2

- **Reader open: half-refresh fast path on same-book / same-settings re-entry** ([#117](https://github.com/diogo7dias/crosspoint-reader-DX34/pull/117), `6376368`). EPUB / XTC / TXT readers no longer pay the ~1 s full-refresh cost when re-entering the same book at the same font, size, orientation, theme, and dither. Cold open, different book, font/orientation/theme change still full-refresh — ghost-prone cases stay covered. Returning from menus / bookmarks / TOC and resuming the last book at boot are visibly faster.

- **Fix: per-book theme no longer leaks into global settings.** In-book toggles (orientation long-press, text render mode, custom-font safety reverts) used to call `SETTINGS.saveToFile()` directly, which wrote the per-book theme overlay (font, size, margins, etc.) back to `/.crosspoint/settings.json` as the new global default. Routed all 8 reader-side save sites through a new `ReadingThemeStore::persistContextual()` that picks per-book or global by context. The "random book on boot" toggle inside the reader menu now snapshots the per-book theme, reloads the on-disk globals, flips the flag, saves clean, and restores the overlay. XTC reader also now loads its per-book `reader_settings.json` on open, for parity with EPUB / TXT. Net effect: global defaults are only ever changed via the global Settings UI, exactly as documented.

- **Sleep wallpapers and library images now support `.pxc` (pre-dithered, faster sleep entry).** `.pxc` files appear in the file browser alongside `.bmp`, can be moved to `/sleep`, favorited (`_F.pxc`), renamed, deleted, and viewed full-screen. The home-screen SD card stats popup counts `.pxc` wallpapers and favorites correctly. Sleep entry using a `.pxc` file is noticeably faster than the equivalent `.bmp` because no dithering is done at wake time.

## Active

- [ ] **Flip `useFactoryLUT` default to ON.** Today the toggle ships off in [src/CrossPointSettings.h](src/CrossPointSettings.h). Default-on means new users get the sharper sleep-cover rendering immediately. Existing on-SD cover BMPs were generated with the prior dither + double-bright, so they'll render slightly darker until the user runs Library Refresh — call that out in the release notes for the version that flips it.

- [x] ~~**PXC visibility in MyLibrary file browser.** Today `.pxc` files are only picked up when dropped directly into `/sleep/`. Wiring needed:~~
  ~~- [MyLibraryActivity.cpp](src/activities/home/MyLibraryActivity.cpp): add `isPxcFile()`, expand `isManagedFile()` to OR it. PXC files should appear in the browser list.~~
  ~~- Action menu for PXC files: Move-to-Sleep + Delete + Rename. Skip the BMP-only "Open Image" item (no PXC viewer yet).~~
  ~~- [UITheme.cpp](src/components/UITheme.cpp): map `.pxc` to the existing image icon.~~
  *(Shipped on `feat/pxc-image-parity` — full browser + action menu + viewer + home-stats parity)*

- [x] ~~**`PxcViewerActivity`** for full-screen PXC viewing from the file browser. Model on the `renderPxcSleepScreen` flow in [SleepActivity.cpp](src/activities/boot_sleep/SleepActivity.cpp). Pairs with the browser-visibility item above.~~
  *(Shipped on `feat/pxc-image-parity` as inline `renderPxcImageView` in MyLibraryActivity)*

- [ ] **EPUB factory LUT for image pages.** Today image pages stay on the differential overlay because the upstream full-page approach needs a 48 KB transient `secondaryFrameBuffer` we can't afford on a ~26 KB-free-heap device. Possible paths:
  - Wait for SDK-side support of windowed factory absolute drive (would let us factory-LUT only the image rect, keep BW text untouched).
  - Or improve heap headroom enough that the secondary buffer fits.
  - Either path is a bigger effort than the others on this list.

- [ ] **XTC factory LUT.** Upstream rewrites `lib/Xtc/Xtc.cpp` and adds an `XtcParser`. Big diff. Wait for clearer user demand or a reason to do the parser rewrite for other purposes.

## SDK fork hygiene

- [ ] **Merge `feat/factory-lut` → `main` on `diogo7dias/community-sdk`.** Today the firmware submodule pins commit `9e21cfe` on the `feat/factory-lut` branch; SDK fork's `main` is still at `c0a79fe` (pre-cherry-pick). Build works either way (submodules pin to a SHA, not a branch), but having SDK main canonical with what we depend on is cleaner.

## Not pursuing

- Bluetooth / BLE HID input support. The NimBLE stack, `BleHidManager`, and pairing UI were removed entirely from this fork because the feature was non-functional (BLE button edges were never polled during reader operation) and held ~50–100 KB of RAM the GPIO-only input path doesn't need. Reintroducing it would need a rewrite of the per-tick polling contract.
