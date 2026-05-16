# TODO

Open follow-ups for this firmware. Prioritised top-down. Workflow per item: build → flash → user tests on device → ship in next release. No soak windows, no waiting periods — this is a hobby project, not a paid product.

## Next release inclusions

Items already merged to `main` that should be called out in the release notes for the next version. Move into the release body when cutting the tag, then clear this section.

- **Snappiness pass (commit `2dfbb69`).** `StatusPopup::showConfirmation` hold reduced from 1 s → 250 ms; boot-stage `logHeapStage()` probes gated behind `-DENABLE_BOOT_HEAP_TRACE` (no-op in default builds); removed always-on `[HEAP] sN_*` LOG_DIAG probes from `CrossPointWebServer` begin/stop and `CrossPointWebServerActivity` onEnter/onExit/STA/AP/DNS paths; demoted `Activity::onEnter`/`onExit` heap snapshots from LOG_DIAG to LOG_DBG. Net: 16-line RTC crash-report ring no longer crowded by per-boot/per-activity boilerplate, so panic reports carry more pre-incident context, and `showConfirmation` is no longer a hard 1-second block.
- **SD Card cleanup setting (commit `16c86d0`).** New System-category action "Clean SD Card (Safe)" — removes orphan `*.tmp`, `*.tmp2`, `*.junk-*` files at `/` and `/.crosspoint/`, plus the pre-RFC-#146 `/wifi_report.txt`. Preserves all books, per-book progress/bookmarks, custom fonts, sleep wallpapers, every `.bak` file, every primary store.

## Active

- [ ] **Port upstream `7acc31b` silent-reboot on WiFi activity exit.** Largest stability win on the table — directly attacks the root cause behind 7 recent OTA/web-server heap-fragmentation bandaid commits (`b962c8a`, `0952f61`, `2d3ff19`, `e49aaaf`, `678ebdb`, `e10876a`, `dbc8f0f`). Upstream's analysis: WiFi/LWIP/netif teardown scatters long-lived allocations across the heap, losing ~50 KB of contiguous space that's unrecoverable without a reboot. Their fix: silent ESP.restart() on exit from every WiFi-using activity, with an RTC_NOINIT magic word that survives the reboot and routes setup() back to home (or to the open EPUB for KOSync). Splash is skipped, so visually it looks like a screen refresh.
  - Scope: 9 files. New `src/SilentRestart.{h,cpp}`. Adapt `src/main.cpp` setup() to read+clear the RTC magic before BootActivity creation and route via ActivityRouter (our boot path diverges from upstream's `activityManager.goToBoot`/`goHome`). Wire `silentRestart()` into onExit of: `CrossPointWebServerActivity`, `OpdsBookBrowserActivity`, `CalibreConnectActivity`. Wire `silentRestartToReader()` into `KOReaderSyncActivity::onExit`; add `wifiActivated` bool field to its header (the `esp_wifi_stop()` after the sync result hides WiFi mode from the onExit gate).
  - Daily-driver guardrail: touches every WiFi-using code path. **Each step requires flash + on-device smoke test before the next.** Sequence: (1) add SilentRestart + setup() routing only, verify a manual `silentRestart()` call lands on home; (2) wire one activity at a time; (3) once all 4 are wired and tested, remove the 7 heap-eviction bandaids in a separate cleanup commit (font-cache eviction before TLS, 17 KB heap reserve, deferred renders, etc. — they become belt-and-suspenders once the root cause is fixed).
  - Adaptation notes vs upstream: our fork has no `FontDownloadActivity` (font downloads go via the web server upload path, which is already covered by `CrossPointWebServerActivity`). Our boot path uses `BootActivity` + `ActivityRouter::begin` instead of upstream's direct `activityManager.go*` calls — the silent-reboot branch in setup() should skip BootActivity creation entirely and call `ActivityRouter::begin({RouteId::Home, ""})` or `{RouteId::Reader, APP_STATE.openEpubPath}` directly.

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
