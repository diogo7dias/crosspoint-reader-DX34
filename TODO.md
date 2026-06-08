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

*(Drained into v5.5.5 release notes 2026-06-08: snappier input (80 MHz idle freq + 20 ms idle loop + held-button keepalive); wallpaper rotation fixes (anti-repeat + measured-cost gate that ends the "can't lock" sleep-entry OOM crash); memory-load reductions (streaming JSON load + save, glyph-prewarm scan, /update page served from flash); status-bar font-size setting removed (always large); consolidated /CRASH_INFO.TXT diagnostics; HTML/CSS parser allocation-churn cleanup.)*

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

_Status 2026-06-06: backlog audited end-to-end against code + git. No open actionable items — every prior `[ ]` is now resolved (`[x]`), mitigated (`[~]`), or moved to **Parked / blocked** below. The only residual engineering work (streaming-layout parser) is deferred design, captured under the section-cache item. Completed entries kept below for history._

- [x] **DONE 2026-06-05 (commit 6e53e53f, device-validated, on main): WiFi exit heap leak: QRShare + KOReaderAuth now silent-restart on exit (salvaged from closed RFC #169).** Capture wifiWasUp before teardown, silentRestart() when set, guarded against no-join backout. Mirrors CrossPointWebServerActivity/CalibreConnectActivity. Both activities bring the radio up but skipped the silent-restart-on-exit that every other WiFi activity does, so ~50 KB of LWIP scatter was held until the next reboot. Verified in code:
    - `QRShareActivity.cpp`: `WiFi.mode(WIFI_STA)` at :63, `onExit` does `WIFI_OFF` at :104 but no `silentRestart`.
    - `KOReaderAuthActivity.cpp`: `WiFi.mode(WIFI_STA)` at :57, `onExit` `WIFI_OFF` at :92, no `silentRestart`.
  - Fix: add a silent-restart on exit when the radio actually came up, matching `CrossPointWebServerActivity`/`CalibreConnectActivity` (→ home) — KOReaderAuth is reached from settings so home is fine; QRShare likewise. Small just-code change; the RFC #169 lifecycle-unification extraction was closed as YAGNI, only these two fixes are worth doing. Build → flash → confirm `/heap_report.txt` shows the reclaim after exiting each.

- [x] **DONE (verified on main 2026-06-06, commit `82f12568`): reader-open-OOM escape — escape the open-OOM dead-end, route to library, keep the crash-loop guard.** Reported by `__melomaniac__` on v4.0.0: device bootloops opening a specific big book — flicker, nothing, repeat. `heap_report.txt`: reason `reader-render-oom`, free 19 KB / largest 9 KB / 53% frag of 142 KB total → heap exhaustion, not just fragmentation. Three compounding bugs found + fixed, all present and wired on main:
    - Boot crash-loop guard `readerActivityLoadCount` was persisted via **debounced** `saveToFile()`; the open-OOM silent-restart fires inside `setup()` before the debounce drains, so the increment never reached disk and every power-cycle reopened the poisoned book. → `CrossPointState::saveToFileSync()` + sync flush at the boot increment (`main.cpp`). Durable now.
    - Budget-exhausted (`kMaxConsecutiveAutoRestarts = 2`) stranded the user on a retry-only recovery screen whose retry just re-OOMs. → `giveUpOpenToHome()` (EpubReaderActivity.cpp) routes to the library with a "Low memory" notice at both terminal give-up sites; navigation deferred via `pendingGoHome` (render-task → loop, avoids use-after-free).
    - `onExit` unconditionally reset the guard → going home would let the next boot reopen the bad book. → `openGiveUpExit_` makes `onExit` preserve the guard after a give-up; it clears the instant any book renders successfully.
  - The branch `fix/reader-open-oom-escape` work merged to main; symbols verified present (`saveToFileSync`, `giveUpOpenToHome`, `pendingGoHome`, `openGiveUpExit_`). Does NOT make the oversized book openable on its own — that is handled by the footprint fix (#163/#164) below, device-validated 2026-06-04.

- [x] **Footprint fix so heavy books actually OPEN (follow-up to the escape hotfix above). DONE — RFC #164 + #163 both landed + closed; #164 step 7 (the degrade-on-OOM fix) device-validated 2026-06-04 (a previously-looping OOM book now opens degraded). On local main, NOT pushed/released.** The earlier "CSS parser ~100 KB at render" was wrong — commit `3feac314` already disk-paged CSS; the real remaining peak was the per-paragraph layout working set + glyph prewarm. Resolution:
    - **RFC #164** (CLOSED) — bounded layout-arena (steps 1-6: LayoutArena/DegradeLevel primitives, LayoutEngine shim, DP+word scratch into the arena, DegradePlan plumbing, 24 KB-anchor hoist) + **step 7 degradation seam**: under heap pressure layout sheds hyphenation (<48K) then images (<28K) and render trims glyph prewarm to regular-only (<40K), degrading instead of OOM→restart. Degraded section caches re-layout at Full once the heap recovers (SECTION_FILE_VERSION 23). Greedy SimpleBreak rung deferred (dp[] already arena-bounded → marginal).
    - **RFC #163** (CLOSED) — `MemoryPolicy` deep module: consolidated heap thresholds + recovery ladder; owns the `kLayout*/kRender*` degrade thresholds the pure `layout::layoutLevelFor/renderLevelFor` map consumes.
  - Nice-to-have still open: reproduce `__melomaniac__`'s exact EPUB on the host reader-sim (SimHeap injector, phase-2 EPUB pipeline) to characterise the killer paragraph — but the device-side fix is in and confirmed.

- [x] **DONE 2026-06-05 (commit 2f6b6fd8, device-validated, on main): CSS render LRU byte-budgeted (~32 KB) instead of 24-entry count-cap.** Running byte counter in lruPut, evict-LRU-until-under-budget (keep >=1), reset in clear(); CssStyle is fixed-size so only key length varies. Kept the deque (low risk) — fixed-array LRU + build-time index streaming still open if more frag headroom needed later. On-disk format unchanged.

- [~] **Reader section-cache fragmentation — MITIGATED, no longer fatal. Residual deferred (see below).** Pre-existing memory bug surfaced during v2.3.9 testing (2026-05-17): two on-device crashes from `Section::createSectionFile` allocation pressure (Crash 1 `size=14336` → abort → `RTC_SW_SYS_RST` with a no-op defrag pass; Crash 2 `size=59456` handled as NULL). Root cause: ESP32-C3 heap is non-moving, `releaseMaxResources()` cannot coalesce non-adjacent free blocks, and `createSectionFile` peak allocations push largest-free below the request.

  **What is now in place (verified on main 2026-06-06):** RFC #163 (`MemoryPolicy` — consolidated heap thresholds `kLayout*/kRender*` + the recovery ladder) and RFC #164 (bounded `LayoutArena` via `new (std::nothrow)`, `DegradeLevel` ladder Full→TrimPrewarm→SimpleBreak→NoHyphen→SkipImages, `SECTION_FILE_VERSION 23` so degraded caches re-layout once heap recovers). Layout sheds hyphenation (<48 KB) then images (<28 KB) and render trims glyph prewarm (<40 KB) instead of OOM→restart; the pre-flight gate runs the ladder (anchor drop → `releaseMaxResources` → silent restart → degrade) rather than panicking. A previously-looping OOM book now opens degraded (device-validated 2026-06-04). Crash-and-reboot is gone; the failure mode is now degrade-or-recovery-screen.

  **Residual (deferred, design work, not actionable now):** the `createSectionFile` working-set *peak* (ZIP decompress ~44 KB, expat read buffer, page LUT) still lives outside the bounded arena, and the non-moving-heap defrag pass is still a no-op (one rung of the ladder, by design). The only true fix for the underlying peak is the streaming-layout parser (working set <16 KB at any point — the "streaming-layout work" from the `kMinLargestBlockHardFloor` comment, follow-up to PR #100). Deferred until a book reliably reproduces a non-degradable peak; characterise on the host reader-sim (SimHeap injector + phase-2 EPUB pipeline) first.

- [x] **DONE (verified on main 2026-06-06): silent-restart port confirmed + OTA bandaids already gone.** Commit `380e2395` (WiFi-exit silent-restart) is on main and shipped in published v3.0.0 / v4.0.0, so it has been device-proven across releases. Coverage is broader than the original note claimed: **6** WiFi activities now silent-restart on exit via the unified `net::teardownAndReclaim` helper (QRShare, KOReaderAuth, CrossPointWebServer, KOReaderSync→Reader, CalibreConnect, OpdsBookBrowser).
  - The "drop the 7 OTA bandaids" cleanup is **already done** — but by the esp_http_client / web-updater refactor, not by a dedicated cleanup commit. `b962c8a` reverted the 17 KB reserve (`0952f61`) and the device-side OTA check no longer needs a >16.7 KB contiguous mbedTLS block, so the font-cache evictions (`678ebdb`, `dbc8f0f`, `2d3ff19`) and deferred render (`e49aaaf`) became dead weight and are gone: `OtaUpdateActivity` is now just a "update from your browser" info screen, and `OtaUpdater::checkForUpdate` streams 1 KB chunks via Arduino `WiFiClientSecure` with no eviction/reserve/defer. Verified: grep of both OTA files shows zero evict/reserve/defer artifacts.
  - Intentionally KEPT: the font-cache eviction in `CrossPointWebServer.cpp` (upload path) is general memory-recovery infra (same primitive as the alloc-failure hook in `main.cpp`), not an OTA bandaid, so it stays.

- [x] **DONE / N-A (verified on main 2026-06-06): backport upstream stability hardening** (commits since `cced777`, 2026-04-15). 4 of 5 landed or applicable, 1 N-A:
  - `8377ac9` — non-throwing alloc + scoped cleanup utils → **DONE**: `lib/Memory/Memory.h` present (`makeUniqueNoThrow`, `ScopedCleanup`), adopted via `e595372b`; in use at multiple sites (the memory-hardening branch already hand-converted the same site list). `ScopedCleanup` defined but not yet wired everywhere — optional future tidy, not a stability gap.
  - `3efc863` — CRC32 font-file verify → **N-A**: requires the SD-card-font download feature (`FontDownloadActivity` / `SdCardFont`), which does not exist in this fork. The built-in CPBN loader already smoke-tests via group-0 inflate.
  - `181ed6c` — missing-variant fallback → **DONE**: `EpdFontFamily::getFont()` cascades BOLD_ITALIC → ITALIC → BOLD → REGULAR and never returns null, so no crash on a missing variant.
  - `db3bb85` — advance-table + prewarm fallbacks → **DONE** (built-ins): commit `fd79074d` on main; prewarm path + implicit fallback present for built-in fonts. The SD-font branch of it is N-A (no SD fonts here).
  - `93e81da` — prune missing books → **DONE**: backported as `1de473d4`; `RecentBooksStore::pruneMissing()`/`isMissing()` run on `RecentBooksActivity::onEnter` (kept off the addBook hot path by design).

- [x] ~~**Bump OTA JSON content-length cap from 8 KB → 16 KB** in `src/network/OtaUpdater.cpp`. Long release-note bodies push the GitHub `/releases/latest` JSON over 8192 bytes, which makes `checkForUpdate()` return HTTP_ERROR before the user is even offered the download.~~
  *(Resolved by #143: `checkForUpdate` now streams the response body in 1 KB chunks via Arduino `HTTPClient::getStreamPtr()` directly into the SAX-style `ReleaseJsonParser`. No content-length buffer is allocated, so there's no cap to bump — release-note size no longer affects OTA.)*

- [x] ~~**PXC visibility in MyLibrary file browser.** Today `.pxc` files are only picked up when dropped directly into `/sleep/`. Wiring needed:~~
  ~~- [MyLibraryActivity.cpp](src/activities/home/MyLibraryActivity.cpp): add `isPxcFile()`, expand `isManagedFile()` to OR it. PXC files should appear in the browser list.~~
  ~~- Action menu for PXC files: Move-to-Sleep + Delete + Rename. Skip the BMP-only "Open Image" item (no PXC viewer yet).~~
  ~~- [UITheme.cpp](src/components/UITheme.cpp): map `.pxc` to the existing image icon.~~
  *(Shipped on `feat/pxc-image-parity` — full browser + action menu + viewer + home-stats parity)*

- [x] ~~**`PxcViewerActivity`** for full-screen PXC viewing from the file browser. Model on the `renderPxcSleepScreen` flow in [SleepActivity.cpp](src/activities/boot_sleep/SleepActivity.cpp). Pairs with the browser-visibility item above.~~
  *(Shipped on `feat/pxc-image-parity` as inline `renderPxcImageView` in MyLibraryActivity)*

## Parked / blocked (not actionable now — revisit on trigger)

These are blocked on an external dependency or deliberately waiting for demand; they are NOT open work-in-flight. No code path here is broken — both are enhancements.

- [ ] **EPUB factory LUT for image pages.** *Blocked on heap / SDK.* Today image pages stay on the differential overlay because the upstream full-page approach needs a 48 KB transient `secondaryFrameBuffer` we can't afford on a ~26 KB-free-heap device. Verified still on the overlay path (`ImageBlock::render` decodes straight to framebuffer; no `secondaryFrameBuffer`, no partial windowed-factory work started). Unblocks only when either: SDK adds windowed factory absolute drive (factory-LUT just the image rect), or reader heap headroom grows enough to fit the secondary buffer. Bigger effort than anything else here.

- [ ] **XTC factory LUT.** *Waiting for demand.* `XtcParser` already exists in this fork (`lib/Xtc/Xtc/XtcParser.{h,cpp}`, ~1.3 KLOC, nothrow-safe, streaming load); the upstream parser-rewrite trigger no longer applies. A factory-LUT path for XTC is a big diff with no current user pull — revisit only on clear demand or if a parser change is wanted for another reason.

## Not pursuing

- Bluetooth / BLE HID input support. The NimBLE stack, `BleHidManager`, and pairing UI were removed entirely from this fork because the feature was non-functional (BLE button edges were never polled during reader operation) and held ~50–100 KB of RAM the GPIO-only input path doesn't need. Reintroducing it would need a rewrite of the per-tick polling contract.
