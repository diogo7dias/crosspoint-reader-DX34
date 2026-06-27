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

*(Drained into v5.5.6 release notes 2026-06-09: wallpaper reconcile hardening (truncated batch keeps playlist dirty, trim re-derive capped + probed, reshuffle measured-cost gated); EPUB layout arena→vector spill heap-probed; test-suite cleanup.)*

*(Drained into v5.5.7 release notes 2026-06-13: reader font picker is now a modal vertical list in both settings menus; switching font keeps your line spacing (no reset); status-bar font-size setting removed entirely; value sliders use tap = ±1, quick double-tap = ±10, hold = steady ±1.)*

*(Drained into v5.5.8 release notes 2026-06-14: fixed a reboot-loop where a memory-heavy book that ran out of memory while drawing a page restarted forever instead of giving up — the retry budget was reset every cycle so the cap of 2 was never reached; the reader now also auto-shrinks to the smallest built-in font to open such a book in place, reverting to your chosen font on the next open.)*

*(Drained into v5.5.10 release notes 2026-06-15: reading-theme cap raised 16 → 30; full reading pages now center text by the real glyph ink box instead of nominal line boxes, so the top and bottom margins look even (the font ascender reserves space above caps that the first line never fills, and line-spacing leading sits below the last line — both readers, EPUB + TXT).)*

*(Drained into v5.6.0 release notes 2026-06-24: memory-safe "View sleep wallpapers" list screen; chapter selector page counts limited to current + next 3 with a sanity clamp (no more absurd numbers); home over-limit warning card → numeric keypad to bulk-move random wallpapers from /sleep to /sleep pause; per-folder file counter in the library header.)*

*(Drained into v6.0.0 release notes 2026-06-24: status-bar chapter-title corruption fix (book.bin cursor race + SECTION_FILE_VERSION 23→24, one-time re-layout on first open); TXT render-OOM recovery mirroring the EPUB path; opt-in "pages left in chapter" status-bar item; list-picker for any setting with >2 options (status positions, orientation, refresh frequency, …) instead of click-to-cycle; Quote Screen Style setting (Classic / Terminal) restyling the in-book quote-selection frame, with a banner showing selection length + chapter (the saved-quotes list stays plain Classic); in-book menu reordered into reading-flow order and the rarely-used "Clean Cache + Progress" entry removed.)*

**FONTS (device-validated 2026-06-27, pending next tag):** reader-font set reshuffle. Removed the F25 Bank Printer and Pixel32 reader fonts entirely (enum, helpers, main.cpp objects, both font menus, baked headers, source TTFs, convert scripts, `patch_f25.py`, i18n keys). Added three new reader fonts at the full Georgia size set (10, 12, 13, 14, 15, 16, 17): **ET Book** (ETbb — serif, all four real faces incl. a real bold-italic), **Rosarivo** (Renaissance serif — regular + italic only; no bold face, so bold falls back to regular), **Lato** (humanist sans-serif — all four real faces). Font-picker order is now ChareInk, Bookerly, Georgia, ET Book, Rosarivo, Lato. Flash 77.4% → 90.4% (the three new fonts net of the two removed — ~630 KB headroom left, room for ~1 more 4-face font). Settings migrate safely: a persisted F25 (13) or Pixel32 (15) value normalizes to ChareInk on load (no brick). New per-font convert scripts: `convert-etbb-only.sh`, `convert-rosarivo-only.sh`, `convert-lato-only.sh` (each self-contained, no dedup, never touches other families).

**MEMORY VACCINE (in progress — sequenced program: shed-aware C-seam → OOM fuzz harness → churn reduction):**
- **Step 1 — shed-aware C-buffer seam (device-validated 2026-06-27, pending next tag).** `std::malloc` (and thus `crosspoint::mem::tryMalloc`, the seam for buffers handed to C decoders) does NOT invoke the C++ new-handler, so the global `oomNewHandler` shed-retry net never fired for it — a large untrusted EPUB chapter read would give up under transient fragmentation that one font-cache shed could have cleared. New `crosspoint::mem::tryMallocShed()` (MemoryPolicy.h) is alloc-first (never gates on headroom, so anything raw malloc would accept still succeeds first try) and only sheds + retries once on a null return. Routed the two `ZipFile::readFileToMemory` chapter-read allocations through it (the verified top gap on the page-turn path). Host-tested via a malloc-hook seam (`test_memory_policy`). Other `tryMalloc` sites left as-is: small row buffers (low value) or the font-cache slot itself (shedding the font cache to allocate it is circular).
- **Step 2 — OOM fuzz harness extended to the read path (2026-06-27).** The host reader-sim already fuzzed the ParsedText LAYOUT path under SimHeap fragmentation (`test_reader_sim`, `wouldAbortThrows()==0`). Extended `test_reader_sim_zip` to drive the REAL `ZipFile::readFileToMemory` against the real EPUB fixture under malloc-failure injection (SimHeap models `operator new`, not `std::malloc`, so the C-seam is driven via the step-1 `setTryMallocHookForTest` hook): proves the shed-aware seam recovers after a shed and degrades to graceful-null under true exhaustion — never a crash. Also repaired a build regression step 1 introduced: adding `#include <MemoryPolicy.h>` to `ZipFile.cpp` broke the hand-rolled `test_sim_zip` / `test_sim_parse` envs, which now carry `-I lib/MemoryPolicy` (+ `-I lib/HeapGuard` for `test_sim_zip`).
  - **Step 2b — CI now enforces the whole test suite + OOM sims (2026-06-27).** Previously CI (`.github/workflows/ci.yml`) ran clang-format + cppcheck + `pio run` only — never `pio test` — so 340 host/sim tests fired only on a manual local run. Added a `host-tests` CI job (wired into the `test-status` required-check gate) that runs `pio test -e test_host` (265) + `test_layout` (19) + `test_sim` (6) + `test_sim_zip` (5) + `test_sim_parse` (45). Supporting fixes: `test_ignore` in `[env:test_host]` for the dedicated-env dirs (so test_host is 0-ERRORED); the `test_sim_parse` stale renderer shadow (added no-op `isFontCacheScanning`/`measureTextInk`; re-baselined its fidelity page-count gate 57→37 — bit-rotted since RFC #170, pipeline verified healthy); and replaced the machine-specific absolute `SIM_EPUB_PATH` with the test's relative fallback so the fixture resolves on a CI checkout. The memory vaccine is now auto-injected on every push/PR.
- Step 3 (later, behind the harness) — fragmentation churn reduction: pool/reuse the font glyph cache + image-decoder workspace.

**MERGED (device-validated 2026-06-27, pending next tag):** favoriting a sleep wallpaper no longer makes it reappear on the very next lock. Marking a `/sleep` image favorite renames it (`x.bmp` → `x_F.bmp`); the rotation order buffer still held the old name, so `reconcile()` saw the renamed file as a brand-new upload and spliced it to the FRONT ("new wallpapers on top"), re-showing the just-favorited image instead of advancing. `reconcile()` now recognizes a favorite/unfavorite rename (a "new" file whose `_F`-toggled counterpart is already a rotation entry) and replaces the name in place, so the image keeps its slot. Host-tested (`test_wallpaper_playlist_v2`).

**PENDING (shipped + device-validated 2026-06-25, NOT yet committed/released — drain into the next tag):** four upstream-steal batches off the `upstream/master` tree audit.
- **EPUB image quality.** Floyd-Steinberg dither now the default (`imageDither` FAST→QUALITY, both `CrossPointSettings.h` member init and `JsonSettingsIO` absent-key fallback); progressive-JPEG covers re-enabled by completing our JPEGDEC patch — we shipped only patch 0001 (pMCU redirect, stops the store-fault crash) but not 0002 (the `if (iMCU >= 0)` guard on the two `pMCU[0]` DC writes in `JPEGDecodeMCU_P`), so progressive decode produced Y-DC-clobbered garbage and was skipped → blank covers; `scripts/patch_jpegdec.py` now applies all three guards inline (kept our `.git`-free patcher, more robust than upstream's git-apply); split the single fine-scale into X/Y (`fineScaleFPX/Y`, `invScaleFPX/Y`) so odd aspect ratios don't drop the wrong source rows.
- **Tier 1 — stability + latency.** Recursive SD mutex + `HalFile::Impl::~Impl()` closes the `FsFile` under `StorageLock` (#2135 — our `asyncwriter` task closing a file off-lock raced `SdSpiCard::m_spiActive` → FreeRTOS priority-disinherit panic; plausibly behind real PANIC dumps); OTA TX buffer 8192→1024 + whole-percent progress throttle (#2074, kept RX at 8 KB for our redirecting URL); `GfxRenderer::isFontCacheScanning()` gate skips underline-width measure (#2237) and the whole image decode (#2230 sub-part) during the font-cache prewarm pass; footnote-origin-on-exit/deep-sleep saves `savedPositions[0]` so a book reopens at the link, not buried in endnotes (#2394).
- **Tier 2 — text correctness (cache bump).** NFC-compose body words + book title + TOC titles (#2277, `lib/Utf8/Utf8ComposeTable.h` + `utf8ComposeNfc`); percent-decode internal EPUB asset/footnote/TOC paths (#2249/#2271, `FsHelpers::decodeUriEscapes` at 7 sites); span-id anchor flood cap (#2303, skip `<span>` ids + `MAX_ANCHORS_PER_CHAPTER=1024`); `SECTION_FILE_VERSION` 24→25 + `BOOK_CACHE_VERSION` 6→7 → every book re-indexes once on first open.
- **Tier 4 — polish.** 12 missing HTML 4.01 named entities (#2352, incl. `&middot;`); `<hr>` scene-break rendering (#7accc607 — new `PageHorizontalRule` serialized element + `PageBuilder::addHorizontalRule` since our fork moved page assembly into PageBuilder; rides the Tier-2 cache bump). Bookmark page-indicator (#2372) deferred — see steal backlog.

**STAGED (built + X4-safe-reviewed 2026-06-25, NOT yet flashed/committed — flash on the daily-driver X4, run the regression checklist, then commit per-phase; full X3 behaviour validates when the X3 hardware arrives):** WiFi/hotspot stability + Xteink X3 hardware support.

WiFi / hotspot:
- **WS upload backpressure.** The client now paces itself to the device's SD-confirmed `PROGRESS` (128 KiB unacked window) instead of streaming at WiFi speed with no app-level flow control — that mismatch let the device's lwIP pbufs / heap exhaust during slow-SD writes and the socket reset mid-transfer (the cause of large-book upload drops). Client-only change in `FilesPage.html`; no device concurrency, no snappy-input risk.
- **Hotspot captive portal.** In AP mode `handleNotFound` now 302-redirects every unknown URI to the AP root. The hotspot's wildcard DNS sends the phone's OS connectivity probes (iOS hotspot-detect, Android generate_204) to the device, which used to 404 them → phone declares "no internet" and never opens a browser → "page won't load at all". Sleep-image + EPUB upload already worked over AP; this makes the page actually reach the user.
- **Fonts over hotspot.** Un-stubbed `/fonts` in AP and now serve `opentype.js` + `pako.js` over AP, loaded SEQUENTIALLY on the page (`window.fontLibsReady`) so the two big gzip payloads are never in flight at once — that parallel fetch was the heap deadlock the AP stub guarded against. `jszip` stays AP-pinned (raw EPUB upload doesn't need it).
- **Web status JSON** now reports `device: X3/X4`.

Xteink X3 support (port of upstream's single-binary runtime-detection design into our diverged tree):
- **SDK (`open-x4-sdk` submodule).** Merged upstream's X3-capable `EInkDisplay` (runtime 792×528 / 800×480 via `setDisplayX3()`, `MAX_BUFFER_SIZE` framebuffers, UC81xx X3 waveforms) into our fork, preserving our factory-LUT grayscale. Pushed as `diogo7dias/community-sdk` branch `x3-merge` (`04e773b`); the firmware submodule-pointer bump is still to commit.
- **Phase 1 — runtime resolution.** `HalDisplay` runtime getters; `GfxRenderer` caches panel dims and replaces the `static_assert` fixed-array BW chunk store with a ceil-div `std::vector` (52272 no longer fails to compile); every `DISPLAY_*` ref now reads runtime. X4 byte-identical (800×480, 6×8000 chunks).
- **Phase 2 — detection.** `HalGPIO` I2C fingerprint probe (BQ27220 / DS3231 / QMI8658, value-validated signatures) + NVS cache + override escape-hatch; runs once at boot, restores the X4's GPIO0/20; X4 fallback default.
- **Phase 3 — display.** `setDisplayX3()` gated on `deviceIsX3()`. (X3 grayscale ghosting-resync deferred — our `HalDisplay` lacks the `displayGrayscaleBase` seam.)
- **Phase 4 — battery.** BQ27220 fuel-gauge SOC read on X3 (+ `Current()`-sign USB/charging detect); X4 ADC path untouched.
- **Phase 5 — clock.** `HalClock` (DS3231) + status-bar clock draw + UTC-offset `VALUE` picker (rendered `UTC+H:MM`) + 12/24h format + once-per-boot NTP sync on WiFi connect; all gated on `halClock.isAvailable()`.
- **Phase 6 — tilt page-turn.** `HalTiltSensor` (QMI8658 gyro flick) folded into the reader input snapshot; `tiltPageTurn` setting (Off/Normal/Inverted); the loop `update()` early-returns on X4, preserving the snappy-input baseline.

Deferred until the X3 is in hand: Phase 7 button-hint pixel geometry for the 528-wide portrait + side buttons; X3 grayscale ghosting-resync; full X3 device validation (resolution, detection, clock, tilt, fuel gauge, hotspot uploads). Possible future guard: key the section/book cache on resolution if an SD card is ever moved between an X4 and an X3 (page breaks differ by viewport).

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

- [~] **DONE-pending-flash 2026-06-20: architecture deepening (improve-codebase-architecture skill), two slices — build clean, host tests green, NOT yet flashed.**
  - **#1 reader ink-centering kernel.** Extracted the duplicated vertical ink-centering arithmetic from `EpubReaderActivity::renderContents` and `TxtReaderActivity::renderPage` into a pure, zero-dep header `src/activities/reader/ReaderInkCentering.h` (`crosspoint::reader::inkCenterOffset(inkTop, inkBottom, vpHeight)`). Behavior-preserving (RAM/Flash byte-identical), kills the documented EPUB⇄TXT mirror-fix hazard. 5 new host tests in `test/test_layout_primitives` (`pio test -e test_layout -f test_layout_primitives`, 19/19 pass).
  - **#2 sleep heap-gate unification.** `Wallpaper.cpp::sequentialPlaylistAffordable()` now reads the SAME injected probe (`deps.largestFreeBlockFn`) the V2 playlist's inner gates use, instead of calling the global util directly. One fake now drives both the outer sequential-vs-direct gate and the inner per-alloc probes; previously a scripted heap moved only one. Falls back to the global util when the probe is unset. 17/17 wallpaper host tests pass.
  - Smoke-test on device before shipping: open an EPUB + a TXT, page through full + last pages, confirm even top/bottom margins (centering unchanged); enter sleep, confirm wallpaper rotation still advances.

- [ ] **TXT render-OOM: mirror EPUB's discard-partial-frame + recovery (follow-up to #1 deepening above).** `EpubReaderActivity::renderContents` counts `getDecompressor()->bitmapAllocFailures()` after the actual render pass and, if non-zero, bails BEFORE `displayBuffer()` (the frame is scattered-glyph garbage) → caller routes to silent-restart / `LayoutRecoveryState` recovery. `TxtReaderActivity::renderPage` does the two-pass prewarm but **never checks `bitmapAllocFailures()`** and is `void` with no recovery infra, so on a fragmented heap a TXT page displays corrupted glyphs instead of recovering. Not a one-liner: TXT has no `silentRestart`/`LayoutRecoveryState` machine, so doing this right means porting (or sharing) a render-OOM recovery path into the TXT activity. Scoped OUT of the 2026-06-20 deepening slice to keep the daily-driver render path safe; pick up as its own commit.

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

## Upstream steal backlog (audited 2026-06-25)

Full tree-content audit of `upstream/master` vs this fork (we share no git history — squashed, so compared by file content). Four batches already landed this session (see the PENDING note under Next release inclusions). What's left to steal, prioritised:

### Tier 3 — big OOM levers (large, risky, stage SOLO + device-validate hard)
- [ ] **#2106 tiled grayscale rendering.** Render each grayscale plane band-by-band into ~8 KB scratch instead of cloning the full ~45-50 KB BW framebuffer in `storeBwBuffer()`/`restoreBwBuffer()` (`GfxRenderer.cpp`, used by the `EpubReaderActivity` image-page path). This is our single largest peak-contiguous consumer on image pages → the biggest memory payoff available. Large port (band state in GfxRenderer + DirectPixelWriter + renderCharImpl band-cull + the reader grayscale loop). Adds a little per-page render cost → device-validate page-turn latency (Snappy LAW). Overlaps the "EPUB factory LUT for image pages" Parked item — both target image-page memory.
- [ ] **#2230 streaming band PixelCache for EPUB inline images.** Stream the inline-image cache to disk in a ~24 KB MCU-row band instead of our full-image buffer (`PixelCache.h`, `MAX_CACHE_BYTES = 256 KB`); lets the cache succeed on a fragmented heap so the image decodes ONCE instead of failing + re-decoding (multi-second freeze). Adapt, don't cherry-pick — our converters diverged (own dithering rework + `DirectCacheWriter`). The cheap sub-part (scan-pass `isScanning()` early-out in `ImageBlock::render`) already shipped in Tier 1.

### Deferred Tier 4
- [ ] **#2372 bookmark page-indicator.** A visible "this page is bookmarked" marker. Deferred because our bookmark model is spine/page (`BookmarkStore`) vs upstream's xpath/percentage — divergent — and `ReaderStatusBar` has zero bookmark-draw code, so adding it means threading a flag through the status-bar layout struct + a glyph asset + a placement decision. Cosmetic, lowest value. Pick up as its own small UI task.

### Tier 5 — maybe (need a decision, or conditional on demand)
- [ ] **#2206 custom sleep-timer picker + "Never".** Replaces the fixed `SLEEP_1/5/10/15/30_MIN` enum with 1-min granularity + Never; real settings-schema migration (`sleepTimeout`→`sleepTimeoutMinutes`) → daily-driver guardrail gate before flashing.
- [ ] **#1658 power-button short-press → footnotes.** Adds a 5th value to the short-power-press enum (slot mechanism already exists, `CrossPointSettings.h`). ⚠ touches power-button tap/hold timing → Snappy-LAW device review.
- [ ] **#2308 element-XPath KOReader progress resolver** (+#2245 chapter-start drift). 900-line paragraph-level resolver vs our DocFragment+percentage `ProgressMapper`. Only worth it if cross-device KOReader sync accuracy matters — our own device round-trips fine today.
- [ ] **NTP reader clock** (#cfd3a381, which gates #2359 statusbar-clock-on-left + #2379 auto-wifi-for-clock-sync). No clock subsystem today; C3 has no RTC so it drifts across deep-sleep. Only if you want a clock on the reader — the DS3231 half is X3 hardware (skip), the NTP-over-WiFi + statusbar-draw half is hardware-agnostic.
- [ ] **#2035 Home cover-cache shrink** (~48 KB → ~16 KB region snapshot) — frees ~36 KB contiguous on the Home screen, exactly where HTTPS (sync / OTA-check) needs it. Medium refactor (new GfxRenderer region helpers). Real headroom win, less acute than the reader-path levers.
- [ ] **#2263 minimize CSS string allocations** — string_view + transparent case-insensitive hash on the hot CSS parse path; eliminates ~12k tiny short-lived allocs per page render. Genuine anti-fragmentation/latency win but ~740 lines on the render hot path → land in isolation, device-validate hard.
- [ ] **#2040 close leaked decoder / mDNS handles** — unique_ptr + ScopedCleanup so failed JPEG/PNG decodes and mDNS/DNS teardown don't leak (compounds over a session). Small, low-risk.

### Excluded by the audit (verified, not worth listing as work)
- **feat-bluetooth BLE page-turner remote** — out of scope per "Not pursuing" below (BLE stack was removed from this fork; reintroduction needs a per-tick polling rewrite + ~50-100 KB RAM + a submodule swap to freeink-sdk).
- Hardware-specific upstream branches (feat-m5-paper-color, feat-support-for-m3, feat-touch, feat-sd-themes / feat-freeink-ui), pure i18n translation PRs, and fixes we already carry in equivalent (often stronger) form — progress.bin corruption guard (we have tmp+`.bak`+mirror), JPEGDEC MCU_SKIP, paragraph-indent / justify-nbsp / hanging-indent / grayscale-ghosting (immune by our overlay architecture or already fixed), OpdsParser right-size, non-throwing alloc. All confirmed during the audit.

## Parked / blocked (not actionable now — revisit on trigger)

These are blocked on an external dependency or deliberately waiting for demand; they are NOT open work-in-flight. No code path here is broken — both are enhancements.

- [ ] **EPUB factory LUT for image pages.** *Blocked on heap / SDK.* Today image pages stay on the differential overlay because the upstream full-page approach needs a 48 KB transient `secondaryFrameBuffer` we can't afford on a ~26 KB-free-heap device. Verified still on the overlay path (`ImageBlock::render` decodes straight to framebuffer; no `secondaryFrameBuffer`, no partial windowed-factory work started). Unblocks only when either: SDK adds windowed factory absolute drive (factory-LUT just the image rect), or reader heap headroom grows enough to fit the secondary buffer. Bigger effort than anything else here.

- [ ] **XTC factory LUT.** *Waiting for demand.* `XtcParser` already exists in this fork (`lib/Xtc/Xtc/XtcParser.{h,cpp}`, ~1.3 KLOC, nothrow-safe, streaming load); the upstream parser-rewrite trigger no longer applies. A factory-LUT path for XTC is a big diff with no current user pull — revisit only on clear demand or if a parser change is wanted for another reason.

## Not pursuing

- Bluetooth / BLE HID input support. The NimBLE stack, `BleHidManager`, and pairing UI were removed entirely from this fork because the feature was non-functional (BLE button edges were never polled during reader operation) and held ~50–100 KB of RAM the GPIO-only input path doesn't need. Reintroducing it would need a rewrite of the per-tick polling contract.
