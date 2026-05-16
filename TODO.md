# TODO

Open follow-ups for this firmware. Prioritised top-down. Workflow per item: build → flash → user tests on device → ship in next release. No soak windows, no waiting periods — this is a hobby project, not a paid product.

## Next release inclusions

Items already merged to `main` that should be called out in the release notes for the next version. Move into the release body when cutting the tag, then clear this section.

*(Drained into v2.3.9 release notes 2026-05-17.)*

## Active

- [ ] **Reader section-cache fragmentation — pre-existing memory bug surfaced during v2.3.9 testing on 2026-05-17.** Two on-device crashes during the test session, both from `Section::createSectionFile` allocation pressure:
    - Crash 1 (after ~6 min reading, book A, custom-font Georgia): `[HEAP] alloc-fail size=14336 caps=6144 fn=heap_caps_malloc` → abort() → `RTC_SW_SYS_RST`. Pre-flight gate had logged `largest=25588 below 49152, running defrag pass / post-defrag largest=25588 (was 25588)` — defrag pass was a complete no-op.
    - Crash 2 (after ~47 sec reading, book B, ChareInk): `[HEAP] alloc-fail size=59456 caps=6144` — this one was handled (no abort, just malloc returned NULL). Pre-flight had logged `free=81512 largest=42996 min=26596`; by the time the alloc fired, largest had crumbled to 6 KB.

  Root cause: ESP32-C3 heap is non-moving and `releaseMaxResources()` in `EpubReaderActivity.cpp:323` only clears pageCache + CSS + font-cache, which are already mostly empty during a fresh section load. Freed-but-non-adjacent blocks don't coalesce, so largest_free_block doesn't change. Then peak allocations during `createSectionFile` (ZIP decompress ~44 KB, CSS reload ~100 KB, expat read buffer, page LUT) push largest below the 14 KB / 59 KB request. Crash 1 was a `new X` that throws `std::bad_alloc` and aborts because we build with `-fno-exceptions`.

  Fix needs design work — not a 1-line patch:
    - Pre-allocate the section-load working buffers once at `EpubReaderActivity::onEnter` so they live in a contiguous reserved region (heap pool pattern). Free at onExit.
    - Or switch the layout to a streaming parser whose working set fits in <16 KB at any point (tracked as the "streaming-layout work" mentioned in the `kMinLargestBlockHardFloor` comment, was a follow-up to PR #100).
    - Or replace every `new X` in the section-load path with `new (std::nothrow) X` + null-check so bad_alloc never reaches abort(). Doesn't fix the underlying frag but turns crash-and-reboot into recovery-screen-and-back-out.
    - The `releaseMaxResources()` "defrag pass" comment in EpubReaderActivity.cpp:331 even acknowledges the limitation — "heap is non-moving"; the pass exists more to keep released memory from blocking the NEXT contiguous request than to fix the current one.

  This is the second root cause of "memory issues" the user originally described; silent-restart only fixes WiFi-induced fragmentation. The two are independent.

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
