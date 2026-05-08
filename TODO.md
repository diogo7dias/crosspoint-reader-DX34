# TODO

Open follow-ups for this firmware. Prioritised top-down. Workflow per item: build → flash → user tests on device → ship in next release. No soak windows, no waiting periods — this is a hobby project, not a paid product.

## Next release inclusions

Items already merged to `main` that should be called out in the release notes for the next version. Move into the release body when cutting the tag, then clear this section.

### v2.1.2

- **fix(reader): force book.bin rebuild on upgrade to recover lost chapter titles.** Bumped `BOOK_CACHE_VERSION` 5→6 in [BookMetadataCache.cpp](lib/Epub/Epub/BookMetadataCache.cpp). Some users saw chapters disappear or render as "Unnamed" after upgrading past the upstream parser picks (#128/#129) — the cached `book.bin` was stale relative to current parser output, but no version bump invalidated it. Also wipes `cachePath/sections/` on cache miss in [Epub.cpp](lib/Epub/Epub.cpp) so spine-indexed section files don't outlive the rebuilt `book.bin`. One-time slow re-index per book on first open after upgrade; subsequent opens hit cache as before.

- **fix(ota): split error codes, surface diagnostics, swap to Arduino HTTPClient for check (#143).** OTA failure screens now show a specific reason instead of a single ambiguous "install/partition" line: `OTA_BEGIN_ERROR` / `OTA_INCOMPLETE_ERROR` / `OTA_FINISH_ERROR` for the three install-path failure sites; the underlying esp_err name (or processed/total bytes for incomplete downloads); and a pre-flight `dns=<IP> tcp=OK|FAIL heap=<bytes>` diagnostic line. `checkForUpdate` swapped to Arduino `WiFiClientSecure::setInsecure` + `HTTPClient` to mirror the proven KOSync stack on this heap-tight C3; CA bundle dropped from both check and install paths (matches KOSync's stance, saves ~70 KB of mbedtls cert-parsing pressure during the handshake, and ~64 KB of flash). Future OTA failures self-report on screen, no serial console required.

## Active

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
