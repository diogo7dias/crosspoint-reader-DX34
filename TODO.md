# TODO

Open follow-ups for this firmware. Prioritised top-down. Workflow per item: build → flash → user tests on device → ship in next release. No soak windows, no waiting periods — this is a hobby project, not a paid product.

## Next release inclusions

Items already merged to `main` that should be called out in the release notes for the next version. Move into the release body when cutting the tag, then clear this section.

*(Drained into v2.3.9 release notes 2026-05-17.)*

*(Drained into v2.3.10 release notes 2026-05-17.)*

*(Drained into v3.0.0 release notes 2026-05-26.)*

*(Drained into v3.0.1 release notes 2026-05-27.)*

*(Drained into v3.0.2 release notes 2026-05-31.)*

*(Drained into v4.0.0 release notes 2026-06-02: settings-OOM fix, lifecycle-only progress saves, 2ms input tick. Remaining snappiness lever — cache the ~75ms per-page layout compute — is now tracked in the Active backlog below.)*

<!-- DRAINED v3.0.1
### Pending for next release (v3.0.1 hotfix)

User-reported v3.0.0 panic from @numeronine. Decoded against firmware.elf:

```
abort -> __terminate -> bad_alloc -> operator new -> vector<string>::insert
  -> ParsedText::layoutAndExtractLines (inlined helper)
  -> ChapterHtmlSlimParser::startNewTextBlock:192
```

Heap at section start: free=80812, largest=61428, fragmentation 65%. The
48 KB pre-flight gate passed, layout fragmented the heap further, and a
mid-layout `vector<string>::insert` could not grow.

v3.0.0 (commit 88c3c0a4) probed `ParsedText::addWord` but the same class
has two more `words.insert(...)` loops that were still bare:

- `lib/Epub/Epub/ParsedText.cpp:splitOversizedTokens` (UTF-8 chunk split)
- `lib/Epub/Epub/ParsedText.cpp:expandHyphenationBreaks` (hyphenation sub-tokens)

Both can throw bad_alloc -> abort under `-fno-exceptions`.

**Fix:** same heap-probe-then-set-oom_-and-return pattern as `addWord`,
applied to both insert loops, plus early-return on `oom_` at top of
`layoutAndExtractLines` and after each helper. Both call sites of
`layoutAndExtractLines` in `ChapterHtmlSlimParser` (lines 730, 1085) now
check `hadOom()` after the call and set `parseFailed = true`, mirroring
the existing addWord-OOM handling at lines 123 and 516.

Net: a fragmented-heap layout returns cleanly through the existing
recovery / silent-restart path instead of panicking. Cost: 45 lines, no
RAM/Flash delta vs v3.0.0 (RAM 54.2%, Flash 91.9% unchanged).

**On-device validation completed 2026-05-27:** flashed, paged through
multiple chapters incl. cross-chapter loads, no panic, layout clean.
-->


<!-- DRAINED v3.0.0
### Pending for next release (memory-hardening branch → v3.0.0)

Branch: `memory-hardening`. Builds clean; host tests green; **NOT YET FLASHED**.
User to review + flash + smoke-test on device before merging to `main` and
cutting the v3.0.0 tag.

**Already on branch (pre-2026-05-19):**
- Sequential newest-first sleep playlist + front-splice for new uploads.
- Host-side fragmented-heap test harness (`test/test_memory_harness/`) with
  6 scenarios incl. 500-iteration fuzz under random heap pressure.
- Nothrow + null-check conversion sweep across the bare-`new` sites that
  could abort the firmware under `-fno-exceptions`:
  - lib/Epub: BookMetadataCache, CssParser (the ~100 KB allocation).
  - lib/Epub/Epub/converters: JpegToFramebufferConverter,
    PngToFramebufferConverter (FsFile open).
  - lib/PngToBmpConverter, lib/JpegToBmpConverter: ditherer alloc +
    scaling accumulators.
  - lib/GfxRenderer/Bitmap.cpp: ditherer alloc.
  - lib/GfxRenderer/BitmapHelpers.h: internal error-row buffers inside
    Atkinson1Bit / Atkinson / FloydSteinberg + `valid()` predicate so
    callers can detect constructor-time OOM.
  - lib/KOReaderSync: 3 WiFiClientSecure sites.
  - lib/Xtc: XtcParser.
  - src/activities/reader/ReaderActivity.cpp: Epub / Xtc / Txt construction.
  - src/activities/network: CrossPointWebServer + DNSServer (captive
    portal) + CalibreConnect web server.
  - src/network: HttpDownloader (WiFiClientSecure / WiFiClient),
    CrossPointWebServer (WebServer + WsUploadSession).

  Outcome: bad_alloc -> abort() -> RTC_SW_SYS_RST replaced by
  return-false-and-show-recovery-screen / silent-restart for all the
  large-allocation sites that the on-device crashes were hitting. Three
  layers of defense intact:
  1. EpubReaderActivity heapHeadroomOkForLayout() pre-flight (48 KB gate
     + defrag pass + 20 KB hard-floor silent restart).
  2. Per-allocation nothrow + null-check propagation up to recovery.
  3. Reader-heap fragmentation auto silent-restart (commit `86b5a93`).

**Added 2026-05-26 (code-only session, no device available):**
- `8ff429eb feat(diag): heap report on every silent restart`
  /heap_report.txt dumps free/largest/min/intl + fragmentation% + reset
  reason on every silentRestart{,ToReader} call. Reason string flows
  from each call site ("wifi-exit-CrossPointWebServer", "reader-
  preflight-frag-recovery", etc.) so the report makes clear which exit
  path triggered. Exposes captureHeapSnapshot()/appendHeapSnapshot()
  for future [HEAP] log standardisation. +428 B flash.
- `1de473d4 feat(recents): prune missing books on add + on browse entry`
  Backport upstream PR #1959 (93e81da). addBook and
  RecentBooksActivity::onEnter now drop stale entries whose backing
  files are gone, so a new add can't push out a still-valid book by
  hitting MAX_RECENT_BOOKS on a slot held by a long-deleted file.
- `e595372b feat(util): adopt upstream Memory.h (nothrow + ScopedCleanup)`
  Backport upstream PR #1832 (8377ac9). Header-only utility; no call-
  site migration in this commit (memory-hardening already landed the
  pattern by hand). Canonical home for future cleanup pass and new
  allocation sites.
- `a9e8797d feat(reader): heap reservation anchor for layout pre-flight`
  EpubReaderActivity allocates a 24 KB contiguous block at onEnter
  while the heap is fresh, releases it in the pre-flight gate before
  falling through to releaseMaxResources, and best-effort re-acquires
  after each successful section build. Closes the typical gap between
  a fragmented device's post-defrag largest-free-block (~25-30 KB) and
  the gate's 48 KB target. Costs 24 KB of held heap during reading.
  **Validation needed:** could net positive or negative depending on
  heap dynamics for a given book; on-device measurement required.

**Upstream backports N/A** (DX34 lacks the SD-card-font feature):
- `181ed6c` font-variant fallback — target code (`SdCardFont`,
  `sdCardFonts_`) doesn't exist in DX34.
- `db3bb850` advance-table + prewarm fallbacks — same.
- `3efc8630` font CRC32 verify — DX34 has no `FontDownloadActivity`.

**On-device validation required before merging to main + tagging v3.0.0:**
- Open Wolfe (or heaviest book), read 10+ min, turn pages → expect OOM
  recovery screen or silent-restart-to-reader (good), not reboot/panic.
- WiFi: File Transfer, Calibre, KOSync, OPDS, Web Server — enter, do
  work, exit; silent-restart should kick in cleanly. Check
  `/heap_report.txt` after each exit: free/largest should look healthy.
- Sleep: confirm sequential newest-first wallpaper rotation; upload a
  new wallpaper, next sleep shows it first.
- Heap anchor regression check: ensure normal reading on a fresh-heap
  device (CHAREINK 12, mid-size book) doesn't show new fragmentation
  symptoms vs pre-anchor builds. The 24 KB hold is unconditional.
- /heap_report.txt format sanity: open in a text editor, confirm reason
  strings make sense, fragmentation % is in 0-100, reset reason matches
  expectation.

**External user reports targeted by v3.0.0 (Instagram DM from @numeronine, 2026-05-26):**
- "When I try to open certain books, the firmware seems to reset and the
  book doesn't open. Sometimes opens but crashes on page turn or chapter
  change." Test book: Herman Melville short fiction, Standard Ebooks
  (https://standardebooks.org/ebooks/herman-melville/short-fiction).
  Classic reader-section-cache fragmentation signature; memory-hardening
  branch's three layers (pre-flight gate + nothrow/null-check sweep +
  silent-restart-to-reader) directly target it. Ask user for
  /crash_report.txt to confirm the alloc that tripped, then re-test
  after v3.0.0 lands. /heap_report.txt (new this session) will give
  post-mortem state on any silent reboot the user hits going forward.
- Separately reported by same user: "Browser-based update only goes to
  2.3.8 even though dialog promises 2.3.10". NOT a v3.0.0 fix — root
  cause is the pre-v2.3.0 OTA JSON content-length cap (resolved by
  PR #143 / commit 3cadcee7, first shipped in v2.3.0). The user is
  stuck because their OTA path is the broken one. Workaround: manual
  one-time flash from GitHub Releases page (web /update with local
  .bin, or USB) → OTA works correctly thereafter. Document this in
  the v3.0.0 release notes under "upgrade notes for users on pre-
  v2.3.0 firmware".

**Subsequent cleanup (post-flash, post-v3.0.0):**
- Drop the 7 WiFi heap-fragmentation bandaid commits once silent-restart
  is confirmed working end-to-end on the daily driver. See "Verify
  silent-restart port" under Active below.
-->

## Active

- [ ] **WiFi exit heap leak: QRShare + KOReaderAuth never silent-restart (salvaged from closed RFC #169).** Both activities bring the radio up but skip the silent-restart-on-exit that every other WiFi activity does, so ~50 KB of LWIP scatter is held until the next reboot. Verified in code:
    - `QRShareActivity.cpp`: `WiFi.mode(WIFI_STA)` at :63, `onExit` does `WIFI_OFF` at :104 but no `silentRestart`.
    - `KOReaderAuthActivity.cpp`: `WiFi.mode(WIFI_STA)` at :57, `onExit` `WIFI_OFF` at :92, no `silentRestart`.
  - Fix: add a silent-restart on exit when the radio actually came up, matching `CrossPointWebServerActivity`/`CalibreConnectActivity` (→ home) — KOReaderAuth is reached from settings so home is fine; QRShare likewise. Small just-code change; the RFC #169 lifecycle-unification extraction was closed as YAGNI, only these two fixes are worth doing. Build → flash → confirm `/heap_report.txt` shows the reclaim after exiting each.

- [ ] **Flash + on-device test branch `fix/reader-open-oom-escape` (reader-open-OOM escape), then merge to main.** Reported by `__melomaniac__` on v4.0.0: device bootloops opening a specific big book — flicker, nothing, repeat. `heap_report.txt`: reason `reader-render-oom`, free 19 KB / largest 9 KB / 53% frag of 142 KB total → heap **exhaustion**, not just fragmentation. Three compounding bugs found + fixed (2 commits on branch):
    - Boot crash-loop guard `readerActivityLoadCount` was incremented then persisted via **debounced** `saveToFile()`; the open-OOM silent-restart fires inside `setup()` before the debounce drains, so the increment never reached disk and every power-cycle reopened the poisoned book. → `CrossPointState::saveToFileSync()` + sync flush at the boot increment (`main.cpp`). **Durable now.**
    - Budget-exhausted (`kMaxConsecutiveAutoRestarts = 2`) stranded the user on a retry-only recovery screen whose retry just re-OOMs. → `giveUpOpenToHome()` routes to the library with a brief "Low memory" notice at both terminal give-up sites (pre-flight gate + render-pass glyph OOM); navigation deferred via `pendingGoHome` (render-task → loop, avoids use-after-free).
    - `onExit` unconditionally reset the guard → going home would let the next boot reopen the bad book. → `openGiveUpExit_` makes `onExit` preserve the guard after a give-up; it clears the instant any book renders successfully.
  - Smoke-test on device: normal open/page-turn/menu/sleep unaffected; force the give-up path (temporarily lower a heap threshold) and confirm it lands in the library, and a power-cycle stays out of the bad book. Then merge + ship as a v4.0.x hotfix.
  - **Does NOT make the oversized book openable** — that's the footprint follow-up below.

- [x] **Footprint fix so heavy books actually OPEN (follow-up to the escape hotfix above). DONE — RFC #164 + #163 both landed + closed; #164 step 7 (the degrade-on-OOM fix) device-validated 2026-06-04 (a previously-looping OOM book now opens degraded). On local main, NOT pushed/released.** The earlier "CSS parser ~100 KB at render" was wrong — commit `3feac314` already disk-paged CSS; the real remaining peak was the per-paragraph layout working set + glyph prewarm. Resolution:
    - **RFC #164** (CLOSED) — bounded layout-arena (steps 1-6: LayoutArena/DegradeLevel primitives, LayoutEngine shim, DP+word scratch into the arena, DegradePlan plumbing, 24 KB-anchor hoist) + **step 7 degradation seam**: under heap pressure layout sheds hyphenation (<48K) then images (<28K) and render trims glyph prewarm to regular-only (<40K), degrading instead of OOM→restart. Degraded section caches re-layout at Full once the heap recovers (SECTION_FILE_VERSION 23). Greedy SimpleBreak rung deferred (dp[] already arena-bounded → marginal).
    - **RFC #163** (CLOSED) — `MemoryPolicy` deep module: consolidated heap thresholds + recovery ladder; owns the `kLayout*/kRender*` degrade thresholds the pure `layout::layoutLevelFor/renderLevelFor` map consumes.
  - Nice-to-have still open: reproduce `__melomaniac__`'s exact EPUB on the host reader-sim (SimHeap injector, phase-2 EPUB pipeline) to characterise the killer paragraph — but the device-side fix is in and confirmed.

- [ ] **CSS render LRU: byte-budget it (small "just-code" follow-up, not RFC-worthy).** CSS is already disk-paged (commit `3feac314`), but the render-time style cache is **count-capped at 24 entries** (`LRU_CAP`, `CssParser.h:143`), not byte-capped — so resident bytes drift with selector-string lengths, and each `std::deque` node heap-allocates a key string (fragmentation source on the very heap we protect). Convert to an explicit byte budget (~32 KB) with a fixed-array LRU; optionally stream the build-time index peak (`parseCssFiles` still materializes `rulesBySelector_` once per first-open) via an external-merge cache writer. Contained, single-subsystem, no on-disk format change, host-testable via the existing `CssParserTU` harness. Just code, no RFC.

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
  - `8377ac9` — non-throwing memory allocation + scoped cleanup utils. *Partially covered by the memory-hardening branch (2026-05-19), which converts the same site list to `new (std::nothrow)` + null-check by hand. A future cleanup pass can extract the per-site checks into the upstream helper for consistency.*
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
