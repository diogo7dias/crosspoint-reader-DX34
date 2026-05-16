# TODO

Open follow-ups for this firmware. Prioritised top-down. Workflow per item: build → flash → user tests on device → ship in next release. No soak windows, no waiting periods — this is a hobby project, not a paid product.

## Next release inclusions

Items already merged to `main` that should be called out in the release notes for the next version. Move into the release body when cutting the tag, then clear this section.

- **Snappiness pass (commit `2dfbb69`).** `StatusPopup::showConfirmation` hold reduced from 1 s → 250 ms; boot-stage `logHeapStage()` probes gated behind `-DENABLE_BOOT_HEAP_TRACE` (no-op in default builds); removed always-on `[HEAP] sN_*` LOG_DIAG probes from `CrossPointWebServer` begin/stop and `CrossPointWebServerActivity` onEnter/onExit/STA/AP/DNS paths; demoted `Activity::onEnter`/`onExit` heap snapshots from LOG_DIAG to LOG_DBG. Net: 16-line RTC crash-report ring no longer crowded by per-boot/per-activity boilerplate, so panic reports carry more pre-incident context, and `showConfirmation` is no longer a hard 1-second block.
- **SD Card cleanup setting (commit `16c86d0`).** New System-category action "Clean SD Card (Safe)" — removes orphan `*.tmp`, `*.tmp2`, `*.junk-*` files at `/` and `/.crosspoint/`, plus the pre-RFC-#146 `/wifi_report.txt`. Preserves all books, per-book progress/bookmarks, custom fonts, sleep wallpapers, every `.bak` file, every primary store.
- **Silent-restart on WiFi activity exit (commit `380e239`).** Port of upstream `7acc31b` (PR #1908). Wires `silentRestart()` / `silentRestartToReader()` into the onExit of File Transfer, OPDS Browser, Calibre Connect, and KOReader Sync activities. Clears WiFi/LWIP heap fragmentation (~50 KB recovered) via `ESP.restart()` with an RTC_NOINIT magic that survives the reboot so setup() can route the user back to home (or to the open EPUB for KOSync) without a boot splash. Root-cause fix for the OTA/web-server heap-fragmentation bandaids that accumulated over the past 7 commits.

## Active

- [ ] **Verify silent-restart port (commit `380e239`) on device, then drop the heap-fragmentation bandaids.** The 4 WiFi-using activities now `silentRestart()` on exit, additively (after existing teardown). Once on-device smoke tests confirm:
    - File Transfer → exit → device reboots silently, lands on home, free heap ~50 KB higher than before exit
    - KOSync → upload progress → exit → silent reboot lands back in the open EPUB at the same page
    - OPDS Browser → exit → silent reboot to home
    - Calibre Connect → exit → silent reboot to home
    - Mode-selection back-out (no WiFi joined) does NOT trigger a reboot
  ...then a cleanup commit can remove the now-redundant bandaids in `src/network/CrossPointWebServer.cpp` and `src/network/OtaUpdater.cpp` (font-cache evictions before TLS, 17 KB heap-reserve, deferred renders). The bandaids are the 7 commits `b962c8a`, `0952f61`, `2d3ff19`, `e49aaaf`, `678ebdb`, `e10876a`, `dbc8f0f`. They become belt-and-suspenders once the root cause is fixed.
  - If silent-restart misbehaves, the per-activity rollback is a 1-line `silentRestart()` deletion — the old teardown was kept intact precisely for this. Worst case: revert commit `380e239` wholesale.

- [ ] **Backport upstream stability hardening** (smaller commits since our last sync, `cced777`, 2026-04-15):
  - `8377ac9` — non-throwing memory allocation + scoped cleanup utils. Adopt at OTA/web/font load sites that currently rely on `new` throwing.
  - `3efc863` — CRC32 checksum verification for font files. Catches SD corruption before it crashes the font cache.
  - `181ed6c` — graceful fallback when a font is missing a variant. Today we crash on first glyph lookup if a variant is missing.
  - `db3bb85` — advance-table + prewarm fallbacks. Hardens the prewarm scan path.
  - `93e81da` — prune missing books from recent list. Drops dangling refs so MyLibrary doesn't keep stale entries pointing at deleted files.

- [x] ~~**Bump OTA JSON content-length cap from 8 KB → 16 KB** in `src/network/OtaUpdater.cpp`. Long release-note bodies push the GitHub `/releases/latest` JSON over 8192 bytes, which makes `checkForUpdate()` return HTTP_ERROR before the user is even offered the download.~~
  *(Resolved by #143: `checkForUpdate` now streams the response body in 1 KB chunks via Arduino `HTTPClient::getStreamPtr()` directly into the SAX-style `ReleaseJsonParser`. No content-length buffer is allocated, so there's no cap to bump — release-note size no longer affects OTA.)*

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

## Not pursuing

- Bluetooth / BLE HID input support. The NimBLE stack, `BleHidManager`, and pairing UI were removed entirely from this fork because the feature was non-functional (BLE button edges were never polled during reader operation) and held ~50–100 KB of RAM the GPIO-only input path doesn't need. Reintroducing it would need a rewrite of the per-tick polling contract.
