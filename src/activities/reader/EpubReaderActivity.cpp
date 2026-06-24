#include "EpubReaderActivity.h"

#include <Arduino.h>
#include <EpdFontFamily.h>
#include <Epub/Page.h>
#include <Epub/blocks/ImageBlock.h>
#include <Epub/layout/DegradeLevel.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <climits>
#include <new>
#include <vector>

#include "../../persist/AsyncWriter.h"
#include "BookmarkListActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "MemoryPolicy.h"
#include "QuotesViewerActivity.h"
#include "ReaderCommon.h"
#include "ReaderInkCentering.h"
#include "ReaderLayoutSafety.h"
#include "ReadingThemeStore.h"
#include "ReadingThemesActivity.h"
#include "RecentBooksStore.h"
#include "SilentRestart.h"
#include "activities/network/QRShareActivity.h"
#include "activities/util/ConfirmDialogActivity.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "fonts/CustomBinFontManager.h"
#include "persist/BackupMirror.h"
#include "util/DrawUtils.h"
#include "util/FavoriteImage.h"
#include "util/StatusPopup.h"
#include "util/TransitionFeedback.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
// (skipChapterMs / goHomeMs / confirmDoubleTapMs moved into
// ReaderInputDispatcher as k* thresholds.)
constexpr unsigned long progressSaveDebounceMs = 800;
int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

using ReaderStatusBar::computeStatusBarReservedHeight;
using ReaderStatusBar::computeStatusBarsHeight;
using ReaderStatusBar::computeStatusTextBlockHeight;
using ReaderStatusBar::getStatusBottomInset;
using ReaderStatusBar::getStatusTopInset;
using ReaderStatusBar::normalizeReaderMargins;
using ReaderStatusBar::statusBarItemIsTop;
using ReaderStatusBar::statusTextPositionIsTop;
using ReaderStatusBar::wrapStatusText;

int resolveCurrentTocIndex(const std::shared_ptr<Epub>& epub, const Section* section, const int currentSpineIndex) {
  if (!epub) {
    return -1;
  }

  if (section != nullptr) {
    int bestTocIndex = -1;
    int bestPage = -1;
    for (const int tocIndex : epub->getTocIndexesForSpineIndex(currentSpineIndex)) {
      const auto tocItem = epub->getTocItem(tocIndex);
      if (tocItem.spineIndex != currentSpineIndex || tocItem.anchor.empty()) {
        continue;
      }

      const int tocPage = section->getPageForAnchor(tocItem.anchor);
      if (tocPage >= 0 && tocPage <= section->currentPage && tocPage >= bestPage) {
        bestPage = tocPage;
        bestTocIndex = tocIndex;
      }
    }

    if (bestTocIndex >= 0) {
      return bestTocIndex;
    }
  }

  return epub->getTocIndexForSpineIndex(currentSpineIndex);
}

// Size of the heap reservation anchor allocated at activity entry. Picked
// to cover the gap between the typical post-defrag largest-free-block
// (~25-30 KB on a fragmented device) and the BuildSectionLayout gate (48 KB,
// crosspoint::mem::Op::BuildSectionLayout in MemoryPolicy.h).
// 24 KB matches the upper end of that gap and is small enough that a
// best-effort tryReacquire after a successful layout has a realistic
// chance to succeed for chapter-traversal scenarios.
constexpr size_t kLayoutHeapAnchorBytes = 24 * 1024;
// Re-acquire only when the post-layout largest contiguous block has at
// least this much headroom above the anchor size, so re-grabbing it
// can't push the heap into the floor zone immediately after a successful
// build.
constexpr size_t kAnchorReacquireHeadroom = 8 * 1024;

}  // namespace

void EpubReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (!epub) {
    return;
  }

  // PR #101: ReaderActivity::loadEpub now always skips CSS, so the heap is
  // still fat when the base class's xTaskCreate runs. Pull the CSS index in
  // here, after the 8 KB stack is acquired. For USER style mode the CSS
  // rules aren't consulted, so we skip the work entirely. ensureCssCache
  // failure is non-fatal — Section.cpp will fall back to layout without
  // CSS rules and the user gets a stylistically degraded but readable page.
  if (didEntryFail()) {
    return;
  }

  // Heap reservation anchor. Take a single contiguous 24 KB block now,
  // while the heap is still freshly defragmented from the activity entry
  // (the base class just ran a clean xTaskCreate and the prior activity's
  // releases have coalesced). The pre-flight gate releases this block
  // before falling through to releaseMaxResources(), giving the next
  // section-build malloc 24 KB of guaranteed-contiguous space to claim.
  // Best-effort: a failure here just disables the optimisation; the
  // existing pre-flight / silent-restart paths still catch fragmentation.
  layoutHeapAnchor_ = crosspoint::layout::LayoutArena::create(kLayoutHeapAnchorBytes);
  if (layoutHeapAnchor_.ok()) {
    LOG_DBG("HEAP", "EPUB onEnter:anchor-acquired %u bytes free=%u largest=%u", (unsigned)kLayoutHeapAnchorBytes,
            (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  } else {
    LOG_DIAG("ERS", "EPUB onEnter: layout heap anchor alloc failed (largest=%u) — continuing without",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  }

  if (SETTINGS.readerStyleMode != CrossPointSettings::READER_STYLE_USER) {
    LOG_DBG("HEAP", "EPUB onEnter:before-css free=%u largest=%u", (unsigned)ESP.getFreeHeap(),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (!epub->ensureCssCache(nullptr)) {
      LOG_ERR("EPUB", "Deferred CSS cache load failed — book renders without CSS");
    }
    LOG_DBG("HEAP", "EPUB onEnter:after-css free=%u largest=%u", (unsigned)ESP.getFreeHeap(),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  }

  // Refresh on book enter. The v1.2.0 experiment unconditionally downgraded
  // this to requestHalfRefresh() to shave ~1 s off open time, but hardware
  // capture 2026-04-24 caught the failure mode the original author warned
  // about: the first page on open, and the first page after a mid-book font
  // switch, both showed ghost pixels of the previous screen overlaid on the
  // new render. Half refresh doesn't fully invert the segment drivers, so
  // incompletely-reset pixels bleed through the first buffer write.
  //
  // Compromise: full refresh only when the book path or a layout-/pixel-
  // affecting setting (font family/size, custom font name+pt, orientation,
  // reader style mode, image dither) has changed since the previous enter.
  // Same-book / same-settings re-entry (resume at boot, return from a
  // subactivity that didn't touch fonts) takes the half-refresh fast path,
  // since the previous frame already laid down the same glyph positions
  // and there's nothing to ghost.
  if (ReaderCommon::shouldFullRefreshOnEnter(epub->getPath())) {
    renderer.requestFullRefresh();
  } else {
    renderer.requestHalfRefresh();
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderCommon::applyReaderOrientation(renderer, SETTINGS.orientation);
  EpdFontFamily::setReaderBoldSwapEnabled(RECENT_BOOKS.getBoldSwap(epub->getPath()));
  ImageBlock::setDitherMode(SETTINGS.imageDither);

  // Activate the custom font before the first drawText — chrome (page
  // counter, header) draws in onEnter, which would otherwise log
  // "Font not found" with a broken glyph on the first paint. If the
  // font is corrupt or unloadable, fall back to the default built-in
  // and persist so the next boot doesn't repeat the OOM dance that
  // bricked the device on broken-DEFLATE fonts.
  if (SETTINGS.fontFamily == CrossPointSettings::CUSTOM_FAMILY && !SETTINGS.customFontName.empty()) {
    const bool ok = crosspoint::fonts::CustomBinFontManager::instance().activate(SETTINGS.customFontName,
                                                                                 SETTINGS.customFontSizePt);
    if (!ok) {
      LOG_ERR("ERS", "custom font '%s' failed to activate; reverting to CHAREINK 12", SETTINGS.customFontName.c_str());
      SETTINGS.fontFamily = CrossPointSettings::CHAREINK;
      SETTINGS.fontSize = CrossPointSettings::SIZE_12;
      SETTINGS.customFontName.clear();
      SETTINGS.customFontSizePt = 0;
      // Persist into the active context: per-book file when this revert
      // happens inside an open book, never the global file (otherwise the
      // per-book theme already applied to SETTINGS would leak into globals).
      ReadingThemeStore::persistContextual(epub ? epub->getCachePath() : std::string());
    }
  }

  epub->setupCacheDir();

  int32_t loadedPageCount = -1;
  FsFile f;
  const std::string progPath = epub->getCachePath() + "/progress.bin";
  const std::string bakPath = epub->getCachePath() + "/progress.bin.bak";
  bool opened = Storage.openFileForRead("ERS", progPath, f);
  if (!opened && Storage.exists(bakPath.c_str())) {
    LOG_INF("ERS", "progress.bin missing, recovering from progress.bin.bak");
    opened = Storage.openFileForRead("ERS", bakPath, f);
  }
  if (!opened) {
    // Last resort: try /.crosspoint/backups/ mirror
    const std::string flatName = backup::flatNameForCacheFile(epub->getCachePath(), "progress.bin");
    if (backup::restoreFromMirror(flatName, progPath)) {
      LOG_INF("ERS", "progress.bin recovered from mirror %s", flatName.c_str());
      opened = Storage.openFileForRead("ERS", progPath, f);
    }
  }
  if (opened) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
      loadedPageCount = cachedChapterTotalPageCount;
    }
    f.close();
  }
  const int spineCount = epub->getSpineItemsCount();
  if (spineCount > 0 && currentSpineIndex >= spineCount) {
    currentSpineIndex = spineCount - 1;
    nextPageNumber = UINT16_MAX;
  }

  progressSink_.setCachePath(epub->getCachePath());
  progressSink_.setSpineCount(spineCount);
  progress_.setDebounceMs(progressSaveDebounceMs);
  progress_.seed({static_cast<int32_t>(currentSpineIndex), static_cast<int32_t>(nextPageNumber), loadedPageCount});

  // We may want a better condition to detect if we are opening for the first
  // time. This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Load bookmarks for this book
  bookmarkStore.load(epub->getCachePath());

  // Save current epub as last opened epub and add to recent books
  ReaderCommon::registerRecentBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
  // Generate cover thumbnail for home screen cover layouts
  epub->generateThumbBmp(400);

  // Move book to /recents/ folder on first open from another location
  {
    std::string newPath = RECENT_BOOKS.moveBookToRecents(epub->getPath());
    if (!newPath.empty()) {
      epub->setPath(newPath);
    }
  }

  // ActivityWithSubactivity::onEnter (above) already started the render
  // task, which can call buildStatusBarLayout and read/write
  // statusBarCache_ at any time. Take RenderLock so this reset is not
  // racing the render task's first paint. Other invalidation sites
  // (orientation change, settings reload) are already wrapped in a
  // RenderLock for the same reason; onExit's invalidate runs after the
  // base onExit has joined the render task and needs no lock.
  {
    RenderLock lock(*this);
    invalidateStatusBarCaches();
    clearPageCache();
  }

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  flushProgressIfNeeded(true);
  inputDispatcher_.clearPendingTap();
  highlights_.exit();
  EpdFontFamily::setReaderBoldSwapEnabled(false);
  ActivityWithSubactivity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  if (openGiveUpExit_) {
    // Open was abandoned (heap exhausted): leave readerActivityLoadCount > 0 so
    // the next boot's forcedHome guard routes to the library instead of
    // reopening this un-openable book. The count was made durable at boot
    // (main.cpp), so a power-cycle escapes too. It clears on the next book that
    // opens successfully (see render()).
    LOG_DIAG("ERS", "onExit: preserving crash-loop guard after open give-up");
  } else {
    APP_STATE.readerActivityLoadCount = 0;
    APP_STATE.saveToFile();
  }
  // Snap back the user's chosen font: the emergency render-degrade latch is
  // transient and dies with the book session, so the next open uses the real
  // font again (and re-degrades only if it OOMs again). Never persisted, so the
  // on-disk font was never touched — this just restores the live SETTINGS global.
  SETTINGS.emergencyRenderFontDowngrade = false;
  clearPageCache();
  section.reset();
  epub.reset();
  layoutHeapAnchor_ = crosspoint::layout::LayoutArena();
  invalidateStatusBarCaches();
}

void EpubReaderActivity::invalidateStatusBarCaches() { statusBarCache_.clear(); }

void EpubReaderActivity::clearPageCache() { cache_.detach(); }

// Pre-flight largest-block thresholds (Op::BuildSectionLayout 48 KB gate,
// Op::RebuildSectionFloor 20 KB hard floor) and their hardware-capture
// history now live in lib/MemoryPolicy/MemoryPolicy.h — the single owner of
// heap-pressure thresholds. This activity reaches them via mem::canAfford /
// mem::thresholdFor / mem::kLayoutHardFloorBytes.

void EpubReaderActivity::tryReacquireLayoutHeapAnchor() {
  if (layoutHeapAnchor_.ok()) {
    return;
  }
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (largest < kLayoutHeapAnchorBytes + kAnchorReacquireHeadroom) {
    // Not enough margin — re-grabbing the anchor here would push the heap
    // back below the floor and the very next allocation (status bar build,
    // page render) would trip. Skip; we'll get another chance on the next
    // successful section build, or after a silent-restart-to-reader cycle.
    LOG_DBG("HEAP", "ERS anchor:re-acquire skipped largest=%u below anchor+headroom=%u", (unsigned)largest,
            (unsigned)(kLayoutHeapAnchorBytes + kAnchorReacquireHeadroom));
    return;
  }
  layoutHeapAnchor_ = crosspoint::layout::LayoutArena::create(kLayoutHeapAnchorBytes);
  if (layoutHeapAnchor_.ok()) {
    LOG_DBG("HEAP", "ERS anchor:re-acquired largest-was=%u largest-now=%u", (unsigned)largest,
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  }
}

void EpubReaderActivity::releaseMaxResources() {
  // Drop everything that competes with the EPUB layout heap budget. The
  // page cache and CSS parser are the two biggest wins (~3 KB and ~100 KB
  // respectively). The font cache manager retains decompressed glyph
  // pages for built-in fonts; pages re-decompress cheaply on next render.
  // Status-bar title caches are byte-trivial but their fragmentation
  // contribution at the top of the heap is worth dropping.
  //
  // ESP-IDF's heap allocator coalesces adjacent free blocks inside free()
  // itself, so by the time the clears below return the largest contiguous
  // block already reflects the merged regions. There is no explicit
  // defrag/compact API to call — heap is non-moving.
  //
  // No global wallpaper / cover bitmap cache exists in this codebase
  // (sleep wallpapers are rendered one-shot per cycle), so nothing to
  // drop on that front. If a long-lived bitmap cache is added later
  // (e.g. for home-screen covers), drop it here too.
  clearPageCache();
  if (epub) {
    auto* css = epub->getCssParser();
    if (css) css->clear();
  }
  auto* fcm = renderer.getFontCacheManager();
  if (fcm) fcm->clearCache();
  invalidateStatusBarCaches();
}

bool EpubReaderActivity::heapHeadroomOkForLayout() {
  namespace mem = crosspoint::mem;

  // Both halves of the recovery ladder now live in the host-testable
  // MemoryPolicy (RFC #163): nextRecoveryStep decides the order, runRecoveryLadder
  // runs the loop (re-probe, flag flips, the 48 KB re-check after the anchor
  // drop, the 20 KB hard-floor + restart-budget branch, and the LOG_DIAG trail).
  // This activity supplies only the three impure rungs — they need `this`, the
  // render lock, and the live anchor. The melomaniac-class fragmentation ladder
  // is reproduced on the host in test/test_memory_policy (runRecoveryLadder).
  const mem::RecoverySeed seed{
      /*anchorHeld=*/layoutHeapAnchor_.ok(),
      /*bookOpen=*/true,  // an open EPUB is what silentRestartToReader resumes
      /*restartBudget=*/remainingAutoSilentRestarts(),
  };

  mem::RecoveryActions acts;
  acts.userData = this;
  // Drop the 24 KB arena anchor (allocated at onEnter): the retry's layout finds
  // no arena and runs the std::vector fallback (RFC #164 step 4) in the freed
  // space. Dropping it first can clear the gate without the costlier cache evict.
  acts.releaseAnchor = [](void* p) -> size_t {
    auto* self = static_cast<EpubReaderActivity*>(p);
    self->layoutHeapAnchor_ = crosspoint::layout::LayoutArena();
    return crosspoint::heap::largestFreeBlockBytes();
  };
  acts.releaseMaxResources = [](void* p) -> size_t {
    auto* self = static_cast<EpubReaderActivity*>(p);
    self->releaseMaxResources();  // evicts CSS index + page + font + status caches
    return crosspoint::heap::largestFreeBlockBytes();
  };
  // Last-resort: non-moving C3 fragmentation only clears via reboot.
  // silentRestartToReader routes back to APP_STATE.openEpubPath so the user
  // lands on the same book. A reserve that fails (budget lost between the peek
  // and here) returns false -> the loop falls through to the recovery screen.
  acts.trySilentRestart = [](void* p) -> bool {
    auto* self = static_cast<EpubReaderActivity*>(p);
    if (!tryReserveAutoSilentRestart()) {
      return false;
    }
    LOG_DIAG("ERS", "pre-flight gate: triggering silent restart to clear fragmentation");
    self->persistProgressBeforeRestart();
    silentRestartToReader("reader-preflight-frag-recovery");  // does not return
    return true;                                              // unreachable
  };

  return mem::layoutHeapRecovered(seed, acts);
}

void EpubReaderActivity::showLayoutRecoveryScreen(LayoutRecoveryState newState) {
  layoutRecoveryState_ = newState;
  // Suppress the in-flight Confirm release that opened this screen so the
  // user has to tap *again* to retry — otherwise a long-press on Confirm
  // (which dispatched ensureSection in the first place) would immediately
  // be consumed as the retry trigger and produce a no-op flicker.
  mappedInput.suppressUntilAllReleased();
  renderer.clearScreen();

  // Body strings (e.g. "Tap any key to free memory and try again.") are wider
  // than the screen at UI_12_FONT_ID, so drawCenteredText alone overflows both
  // edges. Wrap to multiple lines and stack vertically.
  constexpr int kSideMargin = 30;
  constexpr int kSectionGap = 20;
  const int maxWidth = renderer.getScreenWidth() - 2 * kSideMargin;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);

  const char* titleStr;
  const char* bodyStr;
  if (newState == LayoutRecoveryState::AwaitingRetryAfterRevert) {
    titleStr = tr(STR_LAYOUT_FONT_REVERTED_TITLE);
    bodyStr = tr(STR_LAYOUT_FONT_REVERTED_BODY);
  } else {
    titleStr = tr(STR_LAYOUT_LOW_MEMORY_TITLE);
    bodyStr = tr(STR_LAYOUT_LOW_MEMORY_BODY);
  }

  auto drawWrapped = [&](int& y, const char* text) {
    const auto lines = ReaderLayoutSafety::wrapText(renderer, UI_12_FONT_ID, text, maxWidth);
    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_12_FONT_ID, y, line.c_str(), true, EpdFontFamily::REGULAR);
      y += lineHeight;
    }
  };

  int y = 300;
  drawWrapped(y, titleStr);
  y += kSectionGap;
  drawWrapped(y, bodyStr);
  y += kSectionGap;
  drawWrapped(y, tr(STR_LAYOUT_RETRY_HINT));
  renderer.displayBuffer();
}

void EpubReaderActivity::giveUpOpenToHome() {
  // The silent-restart budget is spent and the heap is still too small to open
  // this book — restarting cannot help (the book needs the same contiguous
  // memory every time; this is exhaustion, not transient fragmentation). The
  // old behaviour stranded the user on a "tap to retry" screen whose retry just
  // OOMs again. Instead: keep the durable boot crash-loop guard set (onExit
  // checks openGiveUpExit_ and skips its reset) so the NEXT boot force-routes to
  // the library, then bail to home now with a short notice.
  openGiveUpExit_ = true;
  LOG_DIAG("ERS", "open give-up: heap exhausted, returning to library (guard preserved)");
  renderer.clearScreen();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  int y = 290;
  renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_LAYOUT_LOW_MEMORY_TITLE), true, EpdFontFamily::REGULAR);
  y += lineHeight + 12;
  renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_LOADING_LIBRARY), true, EpdFontFamily::REGULAR);
  // displayBuffer() blocks on the e-ink refresh (~600 ms), so the notice is
  // physically visible before we leave the reader — no unsafe delay() needed.
  renderer.displayBuffer();
  // Defer the actual navigation: giveUpOpenToHome runs on the render task (via
  // render()/ensureSectionLoaded), and tearing down this activity here would be
  // a cross-task use-after-free. pendingGoHome is consumed by loop() on the main
  // task next tick — the same deferral the rest of this class uses.
  pendingGoHome = true;
}

std::shared_ptr<Page> EpubReaderActivity::getCachedPage(const int pageIndex) const {
  return cache_.get(pageIndex, currentSpineIndex);
}

std::shared_ptr<Page> EpubReaderActivity::loadAndCachePage(const int pageIndex) {
  if (!section) {
    return {};
  }

  // refreshPageCacheWindow calls this per page in the prefetch window, so
  // on long first-opens it fires often enough to let the 2s reassurance
  // repaint blink the "Opening book..." popup through the wait.
  TransitionFeedback::maybeShowStillWorkingToast(renderer);

  auto page = std::shared_ptr<Page>(section->loadPageFromSectionFile(pageIndex));
  if (!page) {
    return {};
  }

  if (cache_.spineIndex() != currentSpineIndex) {
    cache_.attach(currentSpineIndex);
  }
  cache_.insert(pageIndex, page);
  return page;
}

void EpubReaderActivity::refreshPageCacheWindow(const int centerPage, const std::shared_ptr<Page>& currentPage) {
  if (!section || centerPage < 0 || centerPage >= section->pageCount) {
    cache_.clear();
    return;
  }

  if (cache_.spineIndex() != currentSpineIndex) {
    cache_.attach(currentSpineIndex);
  }

  cache_.refreshWindow(centerPage, currentPage, section->pageCount, [this](int pageIndex) {
    return std::shared_ptr<Page>(section->loadPageFromSectionFile(pageIndex));
  });
}

int EpubReaderActivity::getWrappedStatusBarReserveLineCount(const int usableWidth) {
  if (!epub || usableWidth <= 0) {
    return 1;
  }
  // Composite cache key: (spineIndex, usableWidth, noTitleTruncation). All three must match for
  // the cached reserve-line count to be valid.
  //   spineIndex          — the TOC title set changes per chapter.
  //   usableWidth         — orientation flip or margin changes reflow the wrap.
  //   noTitleTruncation   — toggling the setting changes the line-count policy, not the width.
  // Any mismatch forces a re-measure, which is expensive (see kMaxTocTitlesMeasured below).
  if (statusBarCache_.cachedReserveSpineIndex == currentSpineIndex &&
      statusBarCache_.cachedReserveUsableWidth == usableWidth &&
      statusBarCache_.cachedReserveNoTitleTruncation == SETTINGS.statusBarNoTitleTruncation) {
    return statusBarCache_.cachedReserveTitleLineCount;
  }

  int maxLines = 1;
  auto tocIndexes = epub->getTocIndexesForSpineIndex(currentSpineIndex);
  if (tocIndexes.empty()) {
    const int fallbackIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (fallbackIndex >= 0) {
      tocIndexes.push_back(fallbackIndex);
    }
  }

  // Cap the sample size. Books like Pocket Oracle have hundreds of TOC
  // entries per spine (one per aphorism). Measuring each title against a
  // cold font cache on first-open costs ~140 ms per entry — on a 300-entry
  // spine that's 40+ seconds of blocking work while "Opening book..." sits
  // invisible on screen. The first handful of entries are representative
  // of the max wrap count for the rest, so cap the iteration and move on.
  constexpr size_t kMaxTocTitlesMeasured = 8;
  const size_t toMeasure = std::min(tocIndexes.size(), kMaxTocTitlesMeasured);
  for (size_t i = 0; i < toMeasure; ++i) {
    const int tocIndex = tocIndexes[i];
    const std::string title = epub->formatTocDisplayTitle(tocIndex);
    if (title.empty()) {
      continue;
    }
    const int lineCount =
        static_cast<int>(wrapStatusText(renderer, SETTINGS.getStatusBarFontId(), title, usableWidth).size());
    if (lineCount > maxLines) {
      maxLines = lineCount;
    }
  }

  statusBarCache_.cachedReserveSpineIndex = currentSpineIndex;
  statusBarCache_.cachedReserveUsableWidth = usableWidth;
  statusBarCache_.cachedReserveNoTitleTruncation = SETTINGS.statusBarNoTitleTruncation;
  statusBarCache_.cachedReserveTitleLineCount = maxLines;
  return statusBarCache_.cachedReserveTitleLineCount;
}

const std::vector<std::string>& EpubReaderActivity::getStatusBarTitleLines(const int tocIndex, const int usableWidth,
                                                                           const bool noTitleTruncation,
                                                                           const int maxTitleLineCount) {
  if (statusBarCache_.cachedTitleTocIndex == tocIndex && statusBarCache_.cachedTitleUsableWidth == usableWidth &&
      statusBarCache_.cachedTitleNoTitleTruncation == noTitleTruncation &&
      statusBarCache_.cachedTitleMaxLines == maxTitleLineCount) {
    return statusBarCache_.cachedTitleLines;
  }

  std::string titleText = tr(STR_UNNAMED);
  if (tocIndex >= 0 && epub) {
    titleText = epub->formatTocDisplayTitle(tocIndex);
    if (titleText.empty()) {
      titleText = tr(STR_UNNAMED);
    }
  }

  statusBarCache_.cachedTitleLines = ReaderLayoutSafety::buildTitleLines(
      renderer, SETTINGS.getStatusBarFontId(), titleText, usableWidth, noTitleTruncation, maxTitleLineCount);

  statusBarCache_.cachedTitleTocIndex = tocIndex;
  statusBarCache_.cachedTitleUsableWidth = usableWidth;
  statusBarCache_.cachedTitleNoTitleTruncation = noTitleTruncation;
  statusBarCache_.cachedTitleMaxLines = maxTitleLineCount;
  return statusBarCache_.cachedTitleLines;
}

EpubReaderActivity::StatusBarLayout EpubReaderActivity::buildStatusBarLayout(const int usableWidth,
                                                                             const int topReservedHeight,
                                                                             const int bottomReservedHeight,
                                                                             const int maxTitleLineCount) {
  StatusBarLayout layout;
  layout.usableWidth = ReaderLayoutSafety::clampViewportDimension(usableWidth, ReaderLayoutSafety::kMinViewportWidth,
                                                                  "ERS", "status width");
  layout.topReservedHeight = topReservedHeight;
  layout.bottomReservedHeight = bottomReservedHeight;
  if (!SETTINGS.statusBarEnabled || !section || !epub) {
    return layout;
  }

  const float sectionChapterProg =
      (section->pageCount > 0) ? static_cast<float>(section->currentPage) / section->pageCount : 0.0f;
  layout.bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100.0f;
  layout.chapterProgress =
      (section->pageCount > 0) ? (static_cast<float>(section->currentPage + 1) / section->pageCount) * 100.0f : 0.0f;

  if (SETTINGS.statusBarShowPageCounter) {
    layout.pageCounterText = ReaderCommon::formatPageCounterText(SETTINGS.statusBarPageCounterMode,
                                                                 section->currentPage, section->pageCount);
    layout.pageCounterTextWidth = renderer.getTextWidth(SETTINGS.getStatusBarFontId(), layout.pageCounterText.c_str());
  }
  if (SETTINGS.statusBarShowBookPercentage) {
    char bookPercentageStr[16] = {0};
    snprintf(bookPercentageStr, sizeof(bookPercentageStr), "B:%.0f%%", layout.bookProgress);
    layout.bookPercentageText = bookPercentageStr;
    layout.bookPercentageTextWidth =
        renderer.getTextWidth(SETTINGS.getStatusBarFontId(), layout.bookPercentageText.c_str());
  }
  if (SETTINGS.statusBarShowChapterPercentage) {
    char chapterPercentageStr[16] = {0};
    snprintf(chapterPercentageStr, sizeof(chapterPercentageStr), "C:%.0f%%", layout.chapterProgress);
    layout.chapterPercentageText = chapterPercentageStr;
    layout.chapterPercentageTextWidth =
        renderer.getTextWidth(SETTINGS.getStatusBarFontId(), layout.chapterPercentageText.c_str());
  }

  populateBookPageCounterText(layout);

  if (SETTINGS.statusBarShowPagesLeft && section->pageCount > 0) {
    // Pages remaining to the end of the current chapter. currentPage is 0-based,
    // so the last page yields 0 left.
    const int remaining = std::max(0, section->pageCount - (section->currentPage + 1));
    char buf[24];
    snprintf(buf, sizeof(buf), "%d %s", remaining, tr(STR_STATUS_PAGES_LEFT_LABEL));
    layout.pagesLeftText = buf;
    layout.pagesLeftTextWidth = renderer.getTextWidth(SETTINGS.getStatusBarFontId(), buf);
  }

  if (SETTINGS.statusBarShowChapterTitle) {
    constexpr int titlePadding = 4;
    const int titleWrapWidth = renderer.getScreenWidth() - titlePadding * 2;
    const int tocIndex = section->getTocIndexForPage(section->currentPage);
    layout.titleLines =
        getStatusBarTitleLines(tocIndex, titleWrapWidth, SETTINGS.statusBarNoTitleTruncation, maxTitleLineCount);
    layout.titleLineWidths.reserve(layout.titleLines.size());
    for (const auto& line : layout.titleLines) {
      layout.titleLineWidths.push_back(renderer.getTextWidth(SETTINGS.getStatusBarFontId(), line.c_str()));
    }
  }

  return layout;
}

void EpubReaderActivity::populateBookPageCounterText(StatusBarLayout& layout) const {
  if (!SETTINGS.statusBarShowBookPageCounter || !section || !epub || section->pageCount <= 0) {
    return;
  }
  // Estimate total book pages by extrapolating the current chapter's pages-per-byte ratio to
  // the whole book. Approximate by design: chapters with images or code have different density
  // than prose, so the estimated total can fluctuate as the reader moves between chapter types.
  // We accept that drift rather than pre-indexing every chapter (which would block first-open).
  const size_t bookSize = epub->getBookSize();
  const size_t prevChapterSize = (currentSpineIndex >= 1) ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  const size_t curChapterSize = epub->getCumulativeSpineItemSize(currentSpineIndex) - prevChapterSize;
  if (curChapterSize == 0 || bookSize == 0) {
    return;
  }
  const float pagesPerByte = static_cast<float>(section->pageCount) / static_cast<float>(curChapterSize);
  const int totalEstimatedPages = std::max(1, static_cast<int>(pagesPerByte * static_cast<float>(bookSize) + 0.5f));
  const int currentAbsPage = std::max(
      1, std::min(totalEstimatedPages,
                  static_cast<int>(layout.bookProgress / 100.0f * static_cast<float>(totalEstimatedPages) + 0.5f)));
  char buf[32];
  snprintf(buf, sizeof(buf), "%d/%d", currentAbsPage, totalEstimatedPages);
  layout.bookPageCounterText = buf;
  layout.bookPageCounterTextWidth = renderer.getTextWidth(SETTINGS.getStatusBarFontId(), buf);
}

void EpubReaderActivity::loop() {
  flushProgressIfNeeded(false);

  if (subActivity) {
    loopSubActivity();
    return;
  }

  // Handle pending navigation callbacks (deferred to avoid use-after-free)
  if (pendingGoHome) {
    pendingGoHome = false;
    if (onGoHome) onGoHome();
    return;
  }
  if (pendingGoLibrary) {
    pendingGoLibrary = false;
    if (onGoBack) onGoBack();
    return;
  }
  if (pendingSectionReset) {
    pendingSectionReset = false;
    section.reset();
  }

  // Layout-recovery dialog active: any button release re-enters layout.
  // Suppress all other reader input (page turns, menu) until the user
  // either taps to retry or BACK to go home. Going home falls through to
  // the BACK handler below, which also clears the recovery state.
  if (layoutRecoveryState_ != LayoutRecoveryState::None) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      layoutRecoveryState_ = LayoutRecoveryState::None;
      onGoHome();
      return;
    }
    if (mappedInput.wasAnyReleased()) {
      // Don't set pendingSectionReset here: section was already reset to
      // null by ensureSectionLoaded before showLayoutRecoveryScreen was
      // called, and render() bails on pendingSectionReset, so setting it
      // would make the very requestUpdate we issue here a no-op.
      // The next render() will see section == null and call
      // ensureSectionLoaded, which is what we want.
      layoutRecoveryState_ = LayoutRecoveryState::None;
      requestUpdate();
    }
    return;
  }

  // Highlight mode intercepts all input while active
  if (highlights_.state() != HighlightState::NONE) {
    loopHighlightMode();
    return;
  }

  // Normal-mode reader input is decoded by the pure ReaderInputDispatcher; the
  // activity only executes the returned action (applyEffect). Recovery and
  // highlight are handled by the pre-gates above and return early, so the mode
  // passed here is always Normal.
  crosspoint::reader::ReaderInputSettings inputSettings;
  inputSettings.longPressChapterSkip = SETTINGS.longPressChapterSkip;
  inputSettings.powerIsPageTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN;

  crosspoint::reader::ReaderState inputState;
  inputState.mode = crosspoint::reader::ReaderState::Mode::Normal;
  inputState.inFootnote = footnoteDepth > 0;
  inputState.atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();
  inputState.hasSection = static_cast<bool>(section);

  const auto effect = inputDispatcher_.dispatch(snapshotInput(), inputSettings, inputState);
  if (effect.suppressUntilAllReleased) {
    mappedInput.suppressUntilAllReleased();
  }
  applyEffect(effect);
}

void EpubReaderActivity::loopSubActivity() {
  subActivity->loop();
  // Deferred exit: process after subActivity->loop() returns to avoid use-after-free
  if (pendingSubactivityExit) {
    pendingSubactivityExit = false;
    const bool shouldReloadTheme = pendingThemeReload;
    pendingThemeReload = false;
    exitActivity();
    if (shouldReloadTheme) {
      reloadCurrentSectionForDisplaySettings();
    } else {
      requestUpdate();
    }
  }
  // Deferred go home: process after subActivity->loop() returns to avoid race condition
  if (pendingGoHome) {
    pendingGoHome = false;
    exitActivity();
    if (onGoHome) onGoHome();
  }
}

void EpubReaderActivity::loopHighlightMode() {
  // SHOW_UNDERLINE: wait 3 seconds (HighlightController::kUnderlineTimeoutMs)
  // then save quote and exit.
  if (highlights_.state() == HighlightState::SHOW_UNDERLINE) {
    if (highlights_.underlineTimedOut(millis())) {
      std::string quote = extractQuoteText();
      if (!quote.empty()) {
        saveQuoteToFile(quote);
        StatusPopup::showConfirmation(renderer, tr(STR_QUOTE_SAVED));
      }
      exitHighlightMode();
    }
    return;
  }
  handleHighlightInput();
}

crosspoint::reader::ReaderInput EpubReaderActivity::snapshotInput() {
  using MB = MappedInputManager::Button;
  // Mirror order MUST match ReaderButton (Back, Confirm, Left, Right, Up, Down,
  // Power, PageBack, PageForward). MappedInputManager::Button declares them in
  // exactly that order; this static_assert is the drift tripwire.
  static constexpr MB kMap[crosspoint::reader::kReaderButtonCount] = {
      MB::Back, MB::Confirm, MB::Left, MB::Right, MB::Up, MB::Down, MB::Power, MB::PageBack, MB::PageForward};
  static_assert(static_cast<int>(crosspoint::reader::ReaderButton::Back) == 0 &&
                    static_cast<int>(crosspoint::reader::ReaderButton::PageForward) == 8,
                "ReaderButton order must mirror MappedInputManager::Button");

  crosspoint::reader::ReaderInput in;
  for (int i = 0; i < crosspoint::reader::kReaderButtonCount; ++i) {
    in.pressed[i] = mappedInput.wasPressed(kMap[i]);
    in.released[i] = mappedInput.wasReleased(kMap[i]);
    in.down[i] = mappedInput.isPressed(kMap[i]);
  }
  in.anyPressed = mappedInput.wasAnyPressed();
  in.anyReleased = mappedInput.wasAnyReleased();
  in.heldTimeMs = mappedInput.getHeldTime();
  in.nowMs = millis();
  return in;
}

void EpubReaderActivity::applyEffect(const crosspoint::reader::ReaderInputDispatcher::Result& effect) {
  using A = crosspoint::reader::ReaderAction;
  switch (effect.action) {
    case A::None:
      return;
    case A::OpenMenu:
      openReaderMenu();
      return;
    case A::ToggleTextRenderMode:
      toggleTextRenderMode();
      return;
    case A::LongPressConfirm:
      enterHighlightMode();
      return;
    case A::GoHome:
      onGoHome();
      return;
    case A::RestoreFootnote:
      restoreSavedPosition();
      return;
    case A::ExitRecoveryToHome:
      layoutRecoveryState_ = LayoutRecoveryState::None;
      onGoHome();
      return;
    case A::ExitRecoveryRetry:
      layoutRecoveryState_ = LayoutRecoveryState::None;
      requestUpdate();
      return;
    case A::RequestUpdate:
      requestUpdate();
      return;
    case A::EndOfBookGoHome:
      onGoHome();
      return;
    case A::EndOfBookStay:
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = UINT16_MAX;
      requestUpdate();
      return;
    case A::SkipChapterPrev:
    case A::SkipChapterNext: {
      TransitionFeedback::show(renderer, tr(STR_LOADING));
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex = (effect.action == A::SkipChapterNext) ? currentSpineIndex + 1 : currentSpineIndex - 1;
        // No progress write on chapter skip — saves are lifecycle-only now. The
        // new position lives in currentSpineIndex/nextPageNumber (consumed by the
        // reload below); the post-load render's observe() refreshes the tracker
        // and the next force-flush (menu/exit/sleep) persists it.
        clearPageCache();
        section.reset();
      }
      requestUpdate();
      return;
    }
    case A::PagePrev: {
      // hasSection is guaranteed by the dispatcher for Page* actions.
      if (section->currentPage > 0) {
        section->currentPage--;
        flushProgressIfNeeded(false);
      } else if (currentSpineIndex > 0) {
        TransitionFeedback::show(renderer, tr(STR_LOADING));
        {
          RenderLock lock(*this);
          nextPageNumber = UINT16_MAX;
          currentSpineIndex--;
          // No progress write on a backward chapter cross — lifecycle-only saves.
          clearPageCache();
          section.reset();
        }
      }
      requestUpdate();
      return;
    }
    case A::PageNext: {
      if (section->currentPage < section->pageCount - 1) {
        section->currentPage++;
        addSessionPagesRead();
        flushProgressIfNeeded(false);
      } else {
        TransitionFeedback::show(renderer, tr(STR_LOADING));
        {
          RenderLock lock(*this);
          nextPageNumber = 0;
          const bool hasNextSection = epub && currentSpineIndex + 1 < epub->getSpineItemsCount();
          currentSpineIndex++;
          if (hasNextSection) {
            addSessionPagesRead();
          }
          // No progress write on a forward chapter cross — lifecycle-only saves.
          clearPageCache();
          section.reset();
        }
      }
      requestUpdate();
      return;
    }
  }
}

void EpubReaderActivity::openReaderMenu() {
  const int currentPage = section ? section->currentPage + 1 : 0;
  const int totalPages = section ? section->pageCount : 0;
  float bookProgress = 0.0f;
  if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
    const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
  const int pageNum = section ? section->currentPage : 0;
  const bool isBookmarked = bookmarkStore.has(currentSpineIndex, pageNum);
  const int bmCount = bookmarkStore.count();
  const std::string quotesPath = getQuotesFilePath();
  const bool hasQuotes = !quotesPath.empty() && Storage.exists(quotesPath.c_str());
  boldSwapAtMenuOpen = RECENT_BOOKS.getBoldSwap(epub->getPath());
  flushProgressIfNeeded(true);
  // Free heap headroom before building the menu → themes → settings
  // sub-activity tree. The reader still holds the parsed section, page cache,
  // the ~100 KB CSS parser, the font glyph cache and the 24 KB layout anchor;
  // on a fragmented heap (big books like Stoner) the bare allocations below
  // would OOM and, under -fno-exceptions, abort() → panic reboot straight back
  // into the book — exactly the "can't open the look settings" symptom. The
  // section-layout path guards this with heapHeadroomOkForLayout(); the menu
  // path had no gate. Only drop the caches when actually low on contiguous
  // heap, so a healthy device keeps an instant menu-back (no re-render). They
  // re-warm on menu-back / next render, and progress was just force-flushed
  // above so dropping them is safe.
  if (!crosspoint::mem::canAfford(crosspoint::mem::Op::BuildSectionLayout)) {
    LOG_DIAG("ERS", "menu open: low contiguous heap, releasing caches + anchor for sub-activity");
    if (layoutHeapAnchor_.ok()) {
      layoutHeapAnchor_ = crosspoint::layout::LayoutArena();
    }
    releaseMaxResources();
  }
  exitActivity();
  enterNewActivity(new (std::nothrow) EpubReaderMenuActivity(
      this->renderer, this->mappedInput, epub->getTitle(), epub->getPath(), currentPage, totalPages,
      bookProgressPercent, SETTINGS.orientation, !currentPageFootnotes.empty(), isBookmarked, bmCount, hasQuotes,
      [this](const uint8_t orientation) { onReaderMenuBack(orientation); },
      [this](EpubReaderMenuActivity::MenuAction action) { onReaderMenuConfirm(action); }));
}

void EpubReaderActivity::toggleTextRenderMode() {
  TransitionFeedback::show(renderer, tr(STR_LOADING));
  flushProgressIfNeeded(true);
  SETTINGS.textRenderMode = (SETTINGS.textRenderMode + 1) % CrossPointSettings::TEXT_RENDER_MODE_COUNT;
  if (!ReadingThemeStore::persistContextual(epub ? epub->getCachePath() : std::string())) {
    LOG_ERR("ERS", "Failed to save settings after text render mode toggle");
  }

  uint16_t backupSpine = 0;
  uint16_t backupPage = 0;
  uint16_t backupPageCount = 1;
  if (epub) {
    const uint16_t spineCount = epub->getSpineItemsCount();
    if (section && section->pageCount > 0) {
      backupSpine = currentSpineIndex;
      backupPage = section->currentPage;
      backupPageCount = section->pageCount;
    } else if (spineCount > 0) {
      if (currentSpineIndex >= spineCount) {
        backupSpine = spineCount - 1;
        backupPage = UINT16_MAX;
      } else {
        backupSpine = currentSpineIndex;
      }
    }
  }

  {
    RenderLock lock(*this);
    clearPageCache();
    section.reset();
    saveProgress(backupSpine, backupPage, backupPageCount);
    nextPageNumber = backupPage;
    cachedSpineIndex = backupSpine;
    cachedChapterTotalPageCount = backupPageCount;
  }
  requestUpdate();
}

void EpubReaderActivity::onReaderMenuBack(const uint8_t orientation) {
  exitActivity();
  // Apply the user-selected orientation when the menu is dismissed.
  // This ensures the menu can be navigated without immediately rotating the
  // screen.
  applyOrientation(orientation);
  // If the user toggled Bold Swap while in the menu, the current page's
  // cached layout no longer matches the new glyph advances (Regular and
  // Bold have different widths). Re-lay out the page with the same flow
  // used for theme changes.
  const bool boldSwapNow = RECENT_BOOKS.getBoldSwap(epub->getPath());
  if (boldSwapNow != boldSwapAtMenuOpen) {
    boldSwapAtMenuOpen = boldSwapNow;
    reloadCurrentSectionForDisplaySettings();
    return;
  }
  requestUpdate();
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize %
  // 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once
  // loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  TransitionFeedback::show(renderer, tr(STR_LOADING));
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    clearPageCache();
    section.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::HIGHLIGHT_QUOTE: {
      exitActivity();
      enterHighlightMode();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::VIEW_QUOTES: {
      const std::string quotesPath = getQuotesFilePath();
      if (quotesPath.empty() || !Storage.exists(quotesPath.c_str())) {
        exitActivity();
        requestUpdate();
        break;
      }
      exitActivity();
      enterNewActivity(new (std::nothrow) QuotesViewerActivity(this->renderer, this->mappedInput, quotesPath,
                                                               [this] { pendingSubactivityExit = true; }));
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      // Calculate values BEFORE we start destroying things
      const int spineIdx = currentSpineIndex;
      const int currentTocIndex = section ? section->getTocIndexForPage(section->currentPage)
                                          : resolveCurrentTocIndex(epub, section.get(), currentSpineIndex);
      // Exact page count of the chapter being read, used to estimate every chapter's length.
      const int currentSectionPageCount = section ? section->pageCount : 0;
      const std::string path = epub->getPath();

      // 1. Close the menu
      exitActivity();

      // 2. Open the Chapter Selector
      enterNewActivity(new (std::nothrow) EpubReaderChapterSelectionActivity(
          this->renderer, this->mappedInput, epub, path, spineIdx, currentTocIndex, currentSectionPageCount,
          [this] {
            exitActivity();
            requestUpdate();
          },
          [this](const int tocIndex) {
            const auto tocItem = epub->getTocItem(tocIndex);
            const int newSpineIndex = tocItem.spineIndex;
            if (newSpineIndex < 0) {
              exitActivity();
              requestUpdate();
              return;
            }

            pendingAnchor = tocItem.anchor;
            if (currentSpineIndex != newSpineIndex || section) {
              TransitionFeedback::show(renderer, tr(STR_LOADING));
              currentSpineIndex = newSpineIndex;
              nextPageNumber = 0;
              clearPageCache();
              section.reset();
            }
            exitActivity();
            requestUpdate();
          },
          [this](const int newSpineIndex, const int newPage) {
            if (currentSpineIndex != newSpineIndex || (section && section->currentPage != newPage)) {
              TransitionFeedback::show(renderer, tr(STR_LOADING));
              currentSpineIndex = newSpineIndex;
              nextPageNumber = newPage;
              clearPageCache();
              section.reset();
            }
            exitActivity();
            requestUpdate();
          }));

      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARK_TOGGLE: {
      const int pageNum = section ? section->currentPage : 0;
      const bool alreadyExists = bookmarkStore.has(currentSpineIndex, pageNum);
      if (!alreadyExists && bookmarkStore.count() >= BookmarkStore::MAX_BOOKMARKS) {
        // Full — show brief feedback then return to menu
        StatusPopup::showConfirmation(renderer, tr(STR_BOOKMARKS_FULL));
        exitActivity();
        requestUpdate();
        break;
      }
      bookmarkStore.toggle(currentSpineIndex, pageNum);
      bookmarkStore.save(epub->getCachePath());
      exitActivity();
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARK_LIST: {
      const int spineIdx = currentSpineIndex;
      const int pageNum = section ? section->currentPage : 0;
      exitActivity();
      enterNewActivity(new (std::nothrow) BookmarkListActivity(
          this->renderer, this->mappedInput, epub, bookmarkStore, epub->getCachePath(), spineIdx, pageNum,
          [this] {
            exitActivity();
            requestUpdate();
          },
          [this](const int targetSpine, const int targetPage) {
            // Save current position for "go back" (reuse footnote stack)
            if (section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
              savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
              footnoteDepth++;
            }
            // Jump to bookmark
            if (currentSpineIndex != targetSpine || !section || section->currentPage != targetPage) {
              TransitionFeedback::show(renderer, tr(STR_LOADING));
              currentSpineIndex = targetSpine;
              nextPageNumber = targetPage;
              clearPageCache();
              section.reset();
            }
            exitActivity();
            requestUpdate();
          }));
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      exitActivity();
      enterNewActivity(new (std::nothrow) EpubReaderFootnotesActivity(
          this->renderer, this->mappedInput, currentPageFootnotes,
          [this] {
            // Go back from footnotes list
            exitActivity();
            requestUpdate();
          },
          [this](const char* href) {
            // Navigate to selected footnote
            navigateToHref(href, true);
            exitActivity();
            requestUpdate();
          }));
      break;
    }
    // GO_HOME menu item removed — Back button handles this
    case EpubReaderMenuActivity::MenuAction::THEMES_MENU: {
      exitActivity();
      enterNewActivity(new (std::nothrow) ReadingThemesActivity(
          renderer, mappedInput, epub ? epub->getCachePath() : std::string(), [this](const bool changed) {
            pendingSubactivityExit = true;
            inputDispatcher_.clearPendingTap();
            pendingThemeReload = changed;
          }));
      break;
    }
    // REVERT_THEME menu item removed — use Reading Themes to manage themes
    case EpubReaderMenuActivity::MenuAction::TRIAGE_FAVORITE: {
      const std::string lastPath = APP_STATE.lastSleepWallpaperPath;
      if (lastPath.empty()) break;
      std::string updatedPath;
      const bool makeFavorite = !FavoriteImage::isFavoritePath(lastPath);
      const auto result = FavoriteImage::setFavorite(lastPath, makeFavorite, &updatedPath);
      if (result == FavoriteImage::SetFavoriteResult::LimitReached) {
        StatusPopup::showConfirmation(renderer, FavoriteImage::limitReachedPopupMessage());
      } else if (result == FavoriteImage::SetFavoriteResult::RenameConflict) {
        StatusPopup::showConfirmation(renderer, tr(STR_FAVORITE_NAME_EXISTS));
      } else if (result != FavoriteImage::SetFavoriteResult::Success) {
        StatusPopup::showConfirmation(renderer, tr(STR_FAVORITE_FAILED));
      } else {
        StatusPopup::showConfirmation(renderer, makeFavorite ? tr(STR_FAVORITED) : tr(STR_UNFAVORITED));
      }
      exitActivity();
      inputDispatcher_.clearPendingTap();
      // Input suppression handled by exitActivity()
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TRIAGE_PAUSE_ROTATION: {
      APP_STATE.wallpaperRotationPaused = !APP_STATE.wallpaperRotationPaused;
      APP_STATE.saveToFile();
      StatusPopup::showConfirmation(
          renderer, APP_STATE.wallpaperRotationPaused ? tr(STR_ROTATION_PAUSED) : tr(STR_ROTATION_UNPAUSED));
      exitActivity();
      inputDispatcher_.clearPendingTap();
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TRIAGE_MOVE_PAUSE: {
      const std::string lastPath = APP_STATE.lastSleepWallpaperPath;
      if (lastPath.empty()) break;
      if (lastPath.rfind("/sleep pause/", 0) == 0) {
        StatusPopup::showConfirmation(renderer, tr(STR_ALREADY_IN_SLEEP_PAUSE));
        exitActivity();
        inputDispatcher_.clearPendingTap();
        // Input suppression handled by exitActivity()
        requestUpdate();
        break;
      }
      const std::string destDir = "/sleep pause";
      Storage.mkdir(destDir.c_str());
      const auto slashPos = lastPath.find_last_of('/');
      const std::string filename = (slashPos == std::string::npos) ? lastPath : lastPath.substr(slashPos + 1);
      const std::string dstPath = destDir + "/" + filename;
      FsFile src, dst;
      bool ok = false;
      if (Storage.openFileForRead("TRG", lastPath.c_str(), src) &&
          Storage.openFileForWrite("TRG", dstPath.c_str(), dst)) {
        uint8_t buf[512];
        ok = true;
        while (src.available()) {
          const int n = src.read(buf, sizeof(buf));
          if (n <= 0 || dst.write(buf, n) != n) {
            ok = false;
            break;
          }
        }
        src.close();
        dst.close();
        if (ok) {
          Storage.remove(lastPath.c_str());
          FavoriteImage::replacePathReferences(lastPath, dstPath);
          APP_STATE.wallpaperRotationPaused = false;
          APP_STATE.saveToFile();
        } else {
          Storage.remove(dstPath.c_str());
        }
      } else {
        if (src) src.close();
        if (dst) dst.close();
      }
      StatusPopup::showConfirmation(renderer, ok ? tr(STR_MOVED_TO_SLEEP_PAUSE) : tr(STR_MOVE_FAILED));
      exitActivity();
      inputDispatcher_.clearPendingTap();
      // Input suppression handled by exitActivity()
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TRIAGE_DELETE: {
      const std::string lastPath = APP_STATE.lastSleepWallpaperPath;
      if (lastPath.empty()) break;
      const bool removed = Storage.remove(lastPath.c_str());
      if (removed) {
        FavoriteImage::removePathReferences(lastPath);
        APP_STATE.wallpaperRotationPaused = false;
        APP_STATE.saveToFile();
      }
      StatusPopup::showConfirmation(renderer, removed ? tr(STR_WALLPAPER_DELETED) : tr(STR_DELETE_FAILED));
      exitActivity();
      inputDispatcher_.clearPendingTap();
      // Input suppression handled by exitActivity()
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      exitActivity();
      enterNewActivity(new (std::nothrow) ConfirmDialogActivity(
          renderer, mappedInput, tr(STR_CLEAR_CACHE_CONFIRM),
          [this]() {
            // Confirmed — clear cache and reset progress.
            exitActivity();
            StatusPopup::showBlocking(renderer, tr(STR_CLEARING_BOOK_CACHE));
            {
              RenderLock lock(*this);
              if (epub) {
                const uint16_t resetSpine = 0;
                const uint16_t resetPage = 0;
                const uint16_t resetPageCount = 1;

                section.reset();
                clearPageCache();
                epub->clearCache();
                epub->setupCacheDir();
                saveProgress(resetSpine, resetPage, resetPageCount);

                currentSpineIndex = resetSpine;
                nextPageNumber = resetPage;
                cachedSpineIndex = resetSpine;
                cachedChapterTotalPageCount = resetPageCount;
              }
            }
            pendingGoHome = true;
          },
          [this]() {
            // Cancelled — return to reader.
            exitActivity();
            requestUpdate();
          }));
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      if (KOREADER_STORE.hasCredentials()) {
        const int currentPage = section ? section->currentPage : 0;
        const int totalPages = section ? section->pageCount : 0;
        exitActivity();
        enterNewActivity(new (std::nothrow) KOReaderSyncActivity(
            renderer, mappedInput, epub, epub->getPath(), currentSpineIndex, currentPage, totalPages,
            [this]() {
              // On cancel - defer exit to avoid use-after-free
              pendingSubactivityExit = true;
            },
            [this](int newSpineIndex, int newPage) {
              // On sync complete - update position and defer exit
              if (currentSpineIndex != newSpineIndex || (section && section->currentPage != newPage)) {
                TransitionFeedback::show(renderer, tr(STR_LOADING));
                currentSpineIndex = newSpineIndex;
                nextPageNumber = newPage;
                clearPageCache();
                section.reset();
              }
              pendingSubactivityExit = true;
            }));
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_BOOK: {
      const std::string bookTitle = epub ? epub->getTitle() : "";
      exitActivity();
      enterNewActivity(new (std::nothrow) ConfirmDialogActivity(
          renderer, mappedInput, std::string(tr(STR_DELETE_FROM_DEVICE)) + "\n" + bookTitle,
          [this]() {
            exitActivity();
            std::string deletingPath;
            StatusPopup::showBlocking(renderer, tr(STR_DELETING_BOOK));
            {
              RenderLock lock(*this);
              if (epub) {
                deletingPath = epub->getPath();
                clearPageCache();
                section.reset();
                epub->clearCache();
              }
            }
            if (!deletingPath.empty()) {
              RECENT_BOOKS.removeBook(deletingPath);
              if (APP_STATE.openEpubPath == deletingPath) {
                APP_STATE.openEpubPath = "";
                APP_STATE.saveToFile();
              }
              const bool removed = Storage.remove(deletingPath.c_str());
              LOG_DBG("ERS", "Delete book '%s': %s", deletingPath.c_str(), removed ? "ok" : "failed");
            }
            pendingGoLibrary = true;
          },
          [this]() {
            exitActivity();
            requestUpdate();
          }));
      break;
    }
    case EpubReaderMenuActivity::MenuAction::REMOVE_FROM_RECENT: {
      const std::string bookTitle = epub ? epub->getTitle() : "";
      exitActivity();
      enterNewActivity(new (std::nothrow) ConfirmDialogActivity(
          renderer, mappedInput, std::string(tr(STR_REMOVE_FROM_RECENTS_CONFIRM)) + "\n" + bookTitle,
          [this]() {
            exitActivity();
            if (epub) {
              RECENT_BOOKS.removeBook(epub->getPath());
              if (APP_STATE.openEpubPath == epub->getPath()) {
                APP_STATE.openEpubPath = "";
                APP_STATE.saveToFile();
              }
            }
            pendingGoHome = true;
          },
          [this]() {
            exitActivity();
            requestUpdate();
          }));
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SHARE_QR: {
      if (epub) {
        exitActivity();
        enterNewActivity(new (std::nothrow) QRShareActivity(
            renderer, mappedInput,
            [this] {
              exitActivity();
              requestUpdate();
            },
            epub->getPath()));
      }
      break;
    }
    default:
      break;
  }
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  TransitionFeedback::show(renderer, tr(STR_LOADING));

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next
    // launch.
    SETTINGS.orientation = orientation;
    ReadingThemeStore::persistContextual(epub ? epub->getCachePath() : std::string());

    // Update renderer orientation to match the new logical coordinate system.
    ReaderCommon::applyReaderOrientation(renderer, SETTINGS.orientation);

    invalidateStatusBarCaches();

    // Reset section to force re-layout in the new orientation.
    clearPageCache();
    section.reset();
  }
}

void EpubReaderActivity::reloadCurrentSectionForDisplaySettings() {
  TransitionFeedback::show(renderer, tr(STR_LOADING));
  flushProgressIfNeeded(true);
  if (epub && SETTINGS.readerStyleMode == CrossPointSettings::READER_STYLE_HYBRID) {
    const bool showCssProgress = epub->getCssParser() == nullptr || !epub->getCssParser()->hasCache();
    const auto progressCallback = std::function<void(int)>();
    if (!epub->ensureCssCache(progressCallback)) {
      LOG_ERR("ERS", "Failed to prepare CSS cache for hybrid reader style");
    } else if (showCssProgress) {
      TransitionFeedback::dismiss(renderer);
    }
  }
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
      saveProgress(currentSpineIndex, section->currentPage, section->pageCount);
    }
    invalidateStatusBarCaches();
    clearPageCache();
    section.reset();
  }
  requestUpdate();
}

bool EpubReaderActivity::ensureSectionLoaded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (section) {
    return true;
  }

  const auto filepath = epub->getSpineItem(currentSpineIndex).href;
  LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
  // Trace largest before/after Section alloc + clearPageCache. Phase 3
  // hardware capture (2026-04-24) showed `largest` stuck around 45 KB
  // mid-session even after releaseAllCaches freed the font slab —
  // something allocated between book-open and section-build is pinning the
  // top of heap. These snapshots narrow it to a specific lifecycle step.
  LOG_DBG("HEAP", "ERS ensureSection:before-alloc free=%u largest=%u min=%u", (unsigned)ESP.getFreeHeap(),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap());
  auto* sec = new (std::nothrow) Section(epub, currentSpineIndex, renderer);
  if (!sec) {
    LOG_ERR("ERS", "OOM: Section allocation");
    return false;
  }
  section = std::unique_ptr<Section>(sec);
  LOG_DBG("HEAP", "ERS ensureSection:after-alloc free=%u largest=%u min=%u", (unsigned)ESP.getFreeHeap(),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap());
  clearPageCache();
  LOG_DBG("HEAP", "ERS ensureSection:after-clearPageCache free=%u largest=%u min=%u", (unsigned)ESP.getFreeHeap(),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap());

  const uint8_t sectionTextRenderMode = SETTINGS.textRenderMode;
  const bool boldSwapEnabled = RECENT_BOOKS.getBoldSwap(epub->getPath());

  // Per-book ReadingThemes can change SETTINGS.customFontName, so
  // re-activate before layout reads the font id.
  if (SETTINGS.fontFamily == CrossPointSettings::CUSTOM_FAMILY && !SETTINGS.customFontName.empty()) {
    crosspoint::fonts::CustomBinFontManager::instance().activate(SETTINGS.customFontName, SETTINGS.customFontSizePt);
  }
  LOG_DBG("HEAP", "ERS ensureSection:after-activate free=%u largest=%u min=%u", (unsigned)ESP.getFreeHeap(),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap());

  if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                SETTINGS.extraParagraphSpacingLevel, SETTINGS.paragraphAlignment, viewportWidth,
                                viewportHeight, SETTINGS.hyphenationEnabled != 0, SETTINGS.wordSpacingPercent,
                                SETTINGS.firstLineIndentMode, SETTINGS.readerStyleMode, sectionTextRenderMode,
                                boldSwapEnabled)) {
    LOG_DBG("HEAP", "ERS ensureSection:after-loadSectionFile-fail free=%u largest=%u min=%u",
            (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
            (unsigned)ESP.getMinFreeHeap());
    LOG_DBG("ERS", "Cache not found, building...");

    // Pre-flight gate: if the heap's largest contiguous block is below the
    // safety threshold, run a defrag pass first; if still below the hard
    // floor, surface a low-memory retry screen instead of attempting a
    // doomed createSectionFile. The user keeps their custom font selection
    // — only the second-failure path below auto-reverts.
    if (!heapHeadroomOkForLayout()) {
      clearPageCache();
      section.reset();
      // Budget spent + heap still under the hard floor: futile to retry. Bail to
      // the library instead of a retry-only screen.
      giveUpOpenToHome();
      return false;
    }

    // Clear built-in font cache; its pages decompress again cheaply on next
    // render. The custom-font slab no longer needs releasing — the ZIP dict
    // lives in BSS (lib/ZipFile/ZipFile.cpp) and the bitmap slab lives in
    // BSS (lib/BdfFont/CustomFontSharedCache.cpp) as of 2026-04-24, so
    // layout no longer competes with the font subsystem for a contiguous
    // heap block. Removing the release/restore cycle here is what
    // structurally eliminates the "blank first page after font switch"
    // regression — the prior flow tried to re-malloc the 16 KB slab after
    // layout, and on a fragmented heap that malloc routinely failed.
    auto* fcm = renderer.getFontCacheManager();
    if (fcm) fcm->clearCache();

    // Cache-miss means we're about to spend hundreds of ms in
    // createSectionFile anyway, so the per-section-cleanup-walk cost
    // (~5 ms × stale .bin files) is hidden behind that. On cache hits
    // we skip both — orphans stay on SD until the next rebuild. The
    // existing fontId-mismatch detection in loadSectionFile already
    // protects correctness; this pass is purely SD janitorial.
    Section::pruneStaleCachesForFont(epub->getCachePath(), SETTINGS.getReaderFontId());

    auto layoutProgressTick = [this](int) { TransitionFeedback::maybeShowStillWorkingToast(renderer); };
    auto runLayout = [&]() {
      return section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                        SETTINGS.extraParagraphSpacingLevel, SETTINGS.paragraphAlignment, viewportWidth,
                                        viewportHeight, SETTINGS.hyphenationEnabled != 0, SETTINGS.wordSpacingPercent,
                                        SETTINGS.firstLineIndentMode, SETTINGS.readerStyleMode, sectionTextRenderMode,
                                        boldSwapEnabled, layoutProgressTick, &layoutHeapAnchor_);
    };

    bool sectionOk = runLayout();
    if (!sectionOk) {
      // First failure: try once more after releasing every heap-resident
      // cache that competes with layout. The retry is safe because
      // Section::createSectionFile removes any partial /.tmp_<spine>.html
      // before re-attempting stream extraction (Section.cpp:312-314).
      // Most fragmentation-driven failures recover on this retry; only
      // genuine over-budget cases fall through to the auto-revert below.
      LOG_DIAG(
          "ERS",
          "createSectionFile fail (1st) spine=%d fontId=%d family=%d sizePt=%u customFont=%s "
          "free=%u largest=%u min=%u",
          currentSpineIndex, SETTINGS.getReaderFontId(), (int)SETTINGS.fontFamily, (unsigned)SETTINGS.customFontSizePt,
          SETTINGS.customFontName.empty() ? "(none)" : SETTINGS.customFontName.c_str(), (unsigned)ESP.getFreeHeap(),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap());
      releaseMaxResources();
      LOG_DIAG("ERS", "retrying layout after defrag: largest=%u",
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      sectionOk = runLayout();
    }
    if (!sectionOk) {
      LOG_DIAG(
          "ERS",
          "createSectionFile fail (2nd, terminal) spine=%d fontId=%d family=%d sizePt=%u "
          "customFont=%s free=%u largest=%u min=%u",
          currentSpineIndex, SETTINGS.getReaderFontId(), (int)SETTINGS.fontFamily, (unsigned)SETTINGS.customFontSizePt,
          SETTINGS.customFontName.empty() ? "(none)" : SETTINGS.customFontName.c_str(), (unsigned)ESP.getFreeHeap(),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap());
      // Boot-loop safety net: when section layout fails twice AND a custom
      // font is the active family, the user is heading toward a
      // crash-on-every-page-turn pattern (custom font heap + ZIP inflator
      // can't both fit). Revert to the default built-in font and persist
      // so the retry below — and the next reopen — have the heap headroom
      // they need to lay out the chapter.
      if (SETTINGS.fontFamily == CrossPointSettings::CUSTOM_FAMILY) {
        LOG_ERR("ERS", "section layout failed under custom font; reverting to CHAREINK 12");
        crosspoint::fonts::CustomBinFontManager::instance().deactivate();
        SETTINGS.fontFamily = CrossPointSettings::CHAREINK;
        SETTINGS.fontSize = CrossPointSettings::SIZE_12;
        SETTINGS.customFontName.clear();
        SETTINGS.customFontSizePt = 0;
        ReadingThemeStore::persistContextual(epub ? epub->getCachePath() : std::string());
      }
      clearPageCache();
      section.reset();
      // The font has already been reverted (if it was custom). The next
      // ensureSectionLoaded — triggered by the user tapping any key per
      // showLayoutRecoveryScreen — will use getReaderFontId() returning
      // the built-in CHAREINK 12 id, which has full heap headroom and
      // succeeds without further intervention.
      showLayoutRecoveryScreen(LayoutRecoveryState::AwaitingRetryAfterRevert);
      return false;
    }
  } else {
    LOG_DBG("ERS", "Cache found, skipping build...");
  }

  // CSS rules are only needed during section creation (layout). Free them now to reclaim
  // ~100 KB for page rendering and font decompression. They reload from cache automatically
  // if a new section needs building later.
  {
    auto* css = epub->getCssParser();
    if (css) css->clear();
  }

  // Apply any pending cross-section navigation now that we know the new page count.
  if (nextPageNumber == UINT16_MAX) {
    section->currentPage = section->pageCount - 1;
  } else {
    section->currentPage = nextPageNumber;
  }

  // Reader-settings change (font size, line spacing, etc.) rebuilds page layout, which changes
  // the page count. Project the old relative position onto the new page grid so the reader
  // doesn't jump to a random page after a settings change.
  if (cachedChapterTotalPageCount > 0) {
    if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
      const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
      section->currentPage = static_cast<int>(progress * section->pageCount);
    }
    cachedChapterTotalPageCount = 0;  // One-shot: don't re-apply on subsequent renders.
  }

  if (pendingPercentJump && section->pageCount > 0) {
    int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
    if (newPage >= section->pageCount) {
      newPage = section->pageCount - 1;
    }
    section->currentPage = newPage;
    pendingPercentJump = false;
  }

  if (!pendingAnchor.empty()) {
    const int anchorPage = section->getPageForAnchor(pendingAnchor);
    if (anchorPage >= 0 && anchorPage < section->pageCount) {
      section->currentPage = anchorPage;
    }
    pendingAnchor.clear();
  }

  // One more chance to blink "Opening book..." before section-init dismiss — catches slow
  // loadSectionFile on big cached sections.
  TransitionFeedback::maybeShowStillWorkingToast(renderer);
  TransitionFeedback::dismiss(renderer);
  // NOTE: do NOT clear the silent-restart budget here. Section layout completing
  // is not proof the page can be displayed — renderContents() allocates the
  // glyph bitmaps later and can still OOM on a fragmented heap. The budget is
  // cleared only after a frame is verifiably on screen (see render()'s success
  // block). Clearing at layout time reset the budget every open, so a
  // render-OOM book never exhausted its 2-attempt cap and silent-restarted
  // forever (the v5.5.x reader-render-oom bootloop brick).
  // Best-effort: re-grab the heap reservation anchor if there's enough
  // headroom. The next chapter change will then have the same 24 KB
  // contiguous safety net the first one did.
  tryReacquireLayoutHeapAnchor();
  return true;
}

void EpubReaderActivity::render(Activity::RenderLock&& lock) {
  if (!epub || pendingSectionReset) {
    return;
  }

  // Long first-open renders (cached sections on big books) spend seconds
  // inside loadSectionFile + page draw with no other tick points. Fire the
  // reassurance repaint here so the "Opening book..." popup gets refreshed
  // on the 10-second cadence even when createSectionFile's layout ticks
  // don't run.
  TransitionFeedback::maybeShowStillWorkingToast(renderer);

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::REGULAR);
    renderer.displayBuffer();
    return;
  }

  // Apply screen viewable areas and additional padding.
  // Downstream helpers still take the four scalars individually, so we unpack the struct into
  // locals at this boundary. Status-bar reserve is applied further down after resolveStatusBarBudget.
  const auto baseMargins = ReaderLayoutSafety::resolveBaseReaderMargins(
      renderer, SETTINGS.screenMarginTop, SETTINGS.screenMarginBottom, SETTINGS.screenMarginHorizontal,
      SETTINGS.dynamicMargins, SETTINGS.getReaderFontId());
  int orientedMarginTop = baseMargins.top;
  int orientedMarginRight = baseMargins.right;
  int orientedMarginBottom = baseMargins.bottom;
  int orientedMarginLeft = baseMargins.left;
  const int minContentHeight =
      std::max(ReaderLayoutSafety::kMinViewportHeight, renderer.getLineHeight(SETTINGS.getReaderFontId()) * 2);

  const int usableWidth =
      ReaderLayoutSafety::clampViewportDimension(renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight,
                                                 ReaderLayoutSafety::kMinViewportWidth, "ERS", "usable width");
  int statusBarTopReserved = 0;
  int statusBarBottomReserved = 0;
  int resolvedTitleLineCount = SETTINGS.statusBarShowChapterTitle ? 1 : 0;
  if (SETTINGS.statusBarEnabled) {
    const bool showTopStatusTextRow =
        (SETTINGS.statusBarShowBattery && statusTextPositionIsTop(SETTINGS.statusBarBatteryPosition)) ||
        (SETTINGS.statusBarShowPageCounter && statusTextPositionIsTop(SETTINGS.statusBarPageCounterPosition)) ||
        (SETTINGS.statusBarShowBookPercentage && statusTextPositionIsTop(SETTINGS.statusBarBookPercentagePosition)) ||
        (SETTINGS.statusBarShowChapterPercentage &&
         statusTextPositionIsTop(SETTINGS.statusBarChapterPercentagePosition)) ||
        (SETTINGS.statusBarShowBookPageCounter && statusTextPositionIsTop(SETTINGS.statusBarBookPageCounterPosition)) ||
        (SETTINGS.statusBarShowPagesLeft && statusTextPositionIsTop(SETTINGS.statusBarPagesLeftPosition));
    const bool showBottomStatusTextRow =
        (SETTINGS.statusBarShowBattery && !statusTextPositionIsTop(SETTINGS.statusBarBatteryPosition)) ||
        (SETTINGS.statusBarShowPageCounter && !statusTextPositionIsTop(SETTINGS.statusBarPageCounterPosition)) ||
        (SETTINGS.statusBarShowBookPercentage && !statusTextPositionIsTop(SETTINGS.statusBarBookPercentagePosition)) ||
        (SETTINGS.statusBarShowChapterPercentage &&
         !statusTextPositionIsTop(SETTINGS.statusBarChapterPercentagePosition)) ||
        (SETTINGS.statusBarShowBookPageCounter && !statusTextPositionIsTop(SETTINGS.statusBarBookPageCounterPosition)) ||
        (SETTINGS.statusBarShowPagesLeft && !statusTextPositionIsTop(SETTINGS.statusBarPagesLeftPosition));
    int titleLineCount = SETTINGS.statusBarShowChapterTitle ? 1 : 0;
    if (SETTINGS.statusBarShowChapterTitle && SETTINGS.statusBarNoTitleTruncation) {
      constexpr int titlePadding = 4;
      const int titleWrapWidth = renderer.getScreenWidth() - titlePadding * 2;
      titleLineCount = getWrappedStatusBarReserveLineCount(titleWrapWidth);
    }
    const int topTitleLineCount =
        (SETTINGS.statusBarShowChapterTitle && statusBarItemIsTop(SETTINGS.statusBarTitlePosition)) ? titleLineCount
                                                                                                    : 0;
    const int bottomTitleLineCount =
        (SETTINGS.statusBarShowChapterTitle && !statusBarItemIsTop(SETTINGS.statusBarTitlePosition)) ? titleLineCount
                                                                                                     : 0;
    const auto budget = ReaderLayoutSafety::resolveStatusBarBudget(
        renderer, SETTINGS.getStatusBarFontId(), "ERS", renderer.getScreenHeight(), getStatusTopInset(renderer),
        getStatusBottomInset(renderer), SETTINGS.screenMarginTop, SETTINGS.screenMarginBottom, minContentHeight,
        SETTINGS.getStatusBarProgressBarHeight(),
        ReaderLayoutSafety::StatusBarBandConfig{
            .showStatusTextRow = showTopStatusTextRow,
            .showBookProgressBar =
                SETTINGS.statusBarShowBookBar && statusBarItemIsTop(SETTINGS.statusBarBookBarPosition),
            .showChapterProgressBar =
                SETTINGS.statusBarShowChapterBar && statusBarItemIsTop(SETTINGS.statusBarChapterBarPosition),
            .desiredTitleLineCount = topTitleLineCount,
        },
        ReaderLayoutSafety::StatusBarBandConfig{
            .showStatusTextRow = showBottomStatusTextRow,
            .showBookProgressBar =
                SETTINGS.statusBarShowBookBar && !statusBarItemIsTop(SETTINGS.statusBarBookBarPosition),
            .showChapterProgressBar =
                SETTINGS.statusBarShowChapterBar && !statusBarItemIsTop(SETTINGS.statusBarChapterBarPosition),
            .desiredTitleLineCount = bottomTitleLineCount,
        });
    statusBarTopReserved = budget.top.reservedHeight;
    statusBarBottomReserved = budget.bottom.reservedHeight;
    resolvedTitleLineCount =
        statusBarItemIsTop(SETTINGS.statusBarTitlePosition) ? budget.top.titleLineCount : budget.bottom.titleLineCount;
    if (statusBarTopReserved > 0) {
      orientedMarginTop = getStatusTopInset(renderer) + SETTINGS.screenMarginTop + statusBarTopReserved;
    }
    if (statusBarBottomReserved > 0) {
      // When the status bar is present it handles the display bottom inset
      // itself. Use only the display inset + user margin so the gap equals
      // exactly screenMarginBottom (0 = text flush against the status bar).
      orientedMarginBottom = getStatusBottomInset(renderer) + SETTINGS.screenMarginBottom + statusBarBottomReserved;
    }
  }

  const uint16_t viewportWidth = static_cast<uint16_t>(
      ReaderLayoutSafety::clampViewportDimension(renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight,
                                                 ReaderLayoutSafety::kMinViewportWidth, "ERS", "viewport width"));
  const uint16_t viewportHeight = static_cast<uint16_t>(
      ReaderLayoutSafety::clampViewportDimension(renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom,
                                                 minContentHeight, "ERS", "viewport height"));

  // DIAG (spike investigation): time the chapter-load stage, which runs before
  // the GFX clearScreen->displayBuffer timer and so is otherwise invisible.
  const uint32_t tEnsure_diag = millis();
  if (!ensureSectionLoaded(viewportWidth, viewportHeight)) {
    return;
  }
  const uint32_t ensureMs_diag = millis() - tEnsure_diag;

  renderer.clearScreen();
  const StatusBarLayout statusBarLayout =
      buildStatusBarLayout(usableWidth, statusBarTopReserved, statusBarBottomReserved, resolvedTitleLineCount);

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::REGULAR);
    renderStatusBar(statusBarLayout, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    renderer.displayBuffer();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d), clamping", section->currentPage, section->pageCount);
    if (section->currentPage < 0) {
      section->currentPage = 0;
    } else {
      section->currentPage = section->pageCount - 1;
    }
  }

  {
    // DIAG (spike investigation): distinguish a RAM cache hit from an SD page
    // load and time the load, to see whether the ~0.8-1.3s spikes are SD reads
    // on a prefetch miss (vs chapter parse, timed above).
    const uint32_t tLoad_diag = millis();
    auto p = getCachedPage(section->currentPage);
    const bool cacheHit_diag = static_cast<bool>(p);
    if (!p) {
      p = loadAndCachePage(section->currentPage);
    }
    const uint32_t loadMs_diag = millis() - tLoad_diag;
    LOG_DBG("ERS", "stage timings: ensureSection=%ums pageLoad=%ums (cache %s) largestFree=%u", ensureMs_diag,
            loadMs_diag, cacheHit_diag ? "HIT" : "MISS",
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    if (!p) {
      pageLoadFailCount++;
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache (attempt %d)", pageLoadFailCount);
      section->clearCache();
      clearPageCache();
      // Defer section.reset() to loop() — it will rebuild and re-render.
      // Do NOT requestUpdate() here: the render task would race with loop()
      // resetting section, causing a null dereference in buildStatusBarLayout.
      pendingSectionReset = true;
      if (pageLoadFailCount >= 3) {
        LOG_ERR("ERS", "Page load failed %d times, showing error", pageLoadFailCount);
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, "Page load failed", true, EpdFontFamily::REGULAR);
        renderer.displayBuffer();
      }
      return;
    }
    pageLoadFailCount = 0;
    refreshPageCacheWindow(section->currentPage, p);

    // Collect footnotes from the loaded page (copy, not move, to preserve cached page data)
    currentPageFootnotes = p->footnotes;
    const auto start = millis();
    const bool renderOk = renderContents(*p, orientedMarginTop, orientedMarginRight, orientedMarginBottom,
                                         orientedMarginLeft, statusBarLayout);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
    if (!renderOk) {
      // Render-time glyph allocations failed on a fragmented heap: the frame
      // was NOT displayed (it would be scattered-glyph garbage).
      //
      // First-line recovery: if we are not already on the smallest built-in
      // font, latch the transient emergency downgrade and re-lay-out + re-render
      // this section at CHAREINK 12. Its glyph groups are small enough to fit
      // the largest free block that defeated the user's heavier font, so the
      // book opens in place instead of reboot-looping. The latch is never
      // persisted and is cleared on book exit (onExit), so the user's real font
      // returns on the next open and only re-degrades if it OOMs again.
      if (!SETTINGS.emergencyRenderFontDowngrade && SETTINGS.getReaderFontId() != CHAREINK_12_FONT_ID) {
        LOG_DIAG("ERS", "render-oom: latching emergency font downgrade -> CHAREINK 12, re-laying out");
        SETTINGS.emergencyRenderFontDowngrade = true;
        // Flash a brief notice. Mirrors giveUpOpenToHome's OOM-path draw and
        // uses the already-resident UI font, not the book font that just OOMed.
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_LAYOUT_LOW_MEMORY_TITLE), true, EpdFontFamily::REGULAR);
        renderer.displayBuffer();
        // Drop the old-font caches; loop() rebuilds + re-renders at the smaller
        // font on the next tick (same deferral the page-load-fail path uses).
        if (section) section->clearCache();
        clearPageCache();
        pendingSectionReset = true;
        return;
      }
      // Already at the smallest font (or it still cannot fit): mirror the
      // pre-flight gate's recovery — silent-restart to a fresh heap where the
      // page can decompress and render, bounded by the auto-restart loop
      // guard. Once the budget is exhausted, fall through to the user-facing
      // recovery screen instead of reboot-looping. The pre-flight gate only
      // guards section *load*; this closes the gap for OOM during the render
      // pass (gate passed, then the heap fragmented further during layout).
      if (tryReserveAutoSilentRestart()) {
        LOG_DIAG("ERS", "render-oom: triggering silent restart to clear fragmentation");
        persistProgressBeforeRestart();
        silentRestartToReader("reader-render-oom");  // does not return
      }
      LOG_DIAG("ERS", "render-oom: auto-restart budget exhausted, returning to library");
      giveUpOpenToHome();
      return;
    }
    // Render succeeded — a frame is verifiably on screen. NOW (not at section-
    // layout time) clear the silent-restart budget so the next fragmentation
    // crisis (e.g. a heavier chapter) gets a fresh 2-attempt allowance. Gating
    // on render success is what bounds a render-OOM book: it lays its section
    // out fine and only fails in renderContents() above, so a layout-time clear
    // reset the budget every cycle and the cap was never reached. RTC-only, no
    // SD cost, so it runs unconditionally on every good frame.
    clearSilentRestartLoopGuard();
    // Render succeeded — the book is verifiably on screen. Clear the durable
    // boot crash-loop guard (set in main.cpp before launch) so a later
    // power-cycle resumes this book normally instead of force-routing home.
    // Guarded by != 0 so it fires once per open and stays off the page-turn
    // hot path's SD writes.
    if (APP_STATE.readerActivityLoadCount != 0) {
      APP_STATE.readerActivityLoadCount = 0;
      APP_STATE.saveToFileSync();
    }
  }
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  flushProgressIfNeeded(false);  // observes current render position + debounce-flushes
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (!epub || !section || section->pageCount < 2) {
    return;
  }

  // Build the next chapter cache while the penultimate page is on screen.
  if (section->currentPage != section->pageCount - 2) {
    return;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return;
  }

  const uint8_t sectionTextRenderMode = SETTINGS.textRenderMode;
  const bool boldSwapEnabled = RECENT_BOOKS.getBoldSwap(epub->getPath());

  Section nextSection(epub, nextSpineIndex, renderer);
  if (nextSection.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacingLevel, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled != 0, SETTINGS.wordSpacingPercent,
                                  SETTINGS.firstLineIndentMode, SETTINGS.readerStyleMode, sectionTextRenderMode,
                                  boldSwapEnabled)) {
    return;  // Already cached
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d", nextSpineIndex);
  if (!nextSection.createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                     SETTINGS.extraParagraphSpacingLevel, SETTINGS.paragraphAlignment, viewportWidth,
                                     viewportHeight, SETTINGS.hyphenationEnabled != 0, SETTINGS.wordSpacingPercent,
                                     SETTINGS.firstLineIndentMode, SETTINGS.readerStyleMode, sectionTextRenderMode,
                                     boldSwapEnabled, nullptr, &layoutHeapAnchor_)) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  }
}

void EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  // Force-write a specific position (chapter skip, orientation change, etc.)
  // and align the tracker so the next debounced flush doesn't rewrite stale
  // values. Sink owns the 6-byte atomic-write format + bounds clamping.
  const crosspoint::reader::ReaderPosition pos{static_cast<int32_t>(spineIndex), static_cast<int32_t>(currentPage),
                                               static_cast<int32_t>(pageCount)};
  progressSink_.write(pos);
  progress_.seed(pos);

  // Cache the percent in RecentBooksStore so the home screen can render it
  // without re-opening the EPUB. Computation mirrors getEpubPercent in
  // util/BookProgress.cpp -- chapterProgress is currentPage / pageCount,
  // then Epub::calculateProgress folds in spine sizes for the absolute
  // percent. We already have epub + spine + page + count here, so this is
  // ~free vs the cost of loading the EPUB at home entry.
  if (epub && pageCount > 0) {
    const int safePage = (currentPage < 0) ? 0 : (currentPage >= pageCount ? pageCount - 1 : currentPage);
    const float chapterProgress = static_cast<float>(safePage) / static_cast<float>(pageCount);
    const int percent = static_cast<int>(epub->calculateProgress(spineIndex, chapterProgress) * 100.0f + 0.5f);
    RECENT_BOOKS.setPercent(epub->getPath(), percent);
  }
}
void EpubReaderActivity::flushProgressIfNeeded(const bool force) {
  if (!epub || !section || section->pageCount == 0) {
    return;
  }
  const auto now = millis();
  progress_.observe({static_cast<int32_t>(currentSpineIndex), static_cast<int32_t>(section->currentPage),
                     static_cast<int32_t>(section->pageCount)},
                    now);
  // Progress is persisted on lifecycle events only — book close / go home,
  // sleep-lock, menu open, theme & render-mode change, KOReader sync — never on
  // a plain page turn or chapter cross. observe() above keeps the tracker's
  // position current in RAM so the next force-flush writes the latest page; the
  // actual SD write happens only when a caller forces it. Real-book model: we
  // save the page when you close the book, not on every turn. (Trade-off: a
  // hard power loss between lifecycle events loses progress back to the last
  // save; accepted deliberately. silentRestartToReader paths flush first so
  // OOM-recovery reboots still land on the current page — see callers.)
  if (force) {
    progress_.flush(now, /*force=*/true);
  }

  // Keep the home-screen percent cache fresh. setPercent is a no-op when
  // the value hasn't changed (no SD write), so this is cheap on every
  // page turn and only persists when the percent actually advances. The
  // persist now goes via AsyncWriter, so this MUST run before the force
  // drain below — otherwise a recent.json write stays queued and races
  // with epub.reset()/SD close on the onExit path (FreeRTOS SPI mutex
  // assertion in xQueueGenericSend).
  const int safePage = (section->currentPage < 0)                     ? 0
                       : (section->currentPage >= section->pageCount) ? section->pageCount - 1
                                                                      : section->currentPage;
  const float chapterProgress = static_cast<float>(safePage) / static_cast<float>(section->pageCount);
  const int percent = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
  RECENT_BOOKS.setPercent(epub->getPath(), percent);

  if (force) {
    // Force-flush callers (onExit, theme/font change, openReaderMenu, deep
    // sleep) need persistence guarantee. setPercent() above only updated the
    // in-RAM percent and marked it dirty (deferred to avoid SD contention with
    // page loads during reading); enqueue that write now, then drain LAST so
    // the drain catches both the progress write and the recent.json percent.
    RECENT_BOOKS.flushPercentIfDirty();
    ::crosspoint::persist::AsyncWriter::instance().drainBlocking();
  }
}

void EpubReaderActivity::persistProgressBeforeRestart() {
  // silentRestartToReader reboots via ESP.restart() WITHOUT flushing, and
  // page-turn writes are now deferred to lifecycle events. Persist the last
  // successfully rendered position synchronously so the post-reboot reader
  // (which reads progress.bin on the way back in) lands on the current page
  // instead of the last menu-open / book-open. Writes the tracker's
  // lastObserved (set by the last good render's observe()); a no-op when
  // nothing changed since the last save.
  progress_.flush(millis(), /*force=*/true);
  RECENT_BOOKS.flushPercentIfDirty();
  ::crosspoint::persist::AsyncWriter::instance().drainBlocking();
}

void EpubReaderActivity::addSessionPagesRead(const uint32_t amount) { APP_STATE.sessionPagesRead += amount; }

// ── Highlight / Quote selection mode ──────────────────────────────────────────

std::vector<EpubReaderActivity::WordInfo> EpubReaderActivity::buildWordList(const Page& page, const int xOffset,
                                                                            const int yOffset, const int fontId) const {
  std::vector<WordInfo> result;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*el);
    const auto& tb = line.getTextBlock();
    const auto& words = tb.getWords();
    const auto& xpos = tb.getWordXpos();
    const auto& styles = tb.getWordStyles();
    const int16_t ls = tb.getLetterSpacing();
    for (size_t i = 0; i < words.size(); i++) {
      WordInfo wi;
      wi.x = static_cast<int>(xpos[i]) + line.xPos + xOffset;
      wi.y = line.yPos + yOffset;
      wi.width = renderer.getTextWidthSpaced(fontId, words[i].c_str(), ls, styles[i]);
      wi.text = words[i];
      wi.style = styles[i];
      wi.letterSpacing = ls;
      result.push_back(std::move(wi));
    }
  }
  return result;
}

void EpubReaderActivity::rebuildHighlightWordCache(const int xOffset, const int yOffset) {
  std::vector<crosspoint::reader::WordPos> words;
  auto page = loadAndCachePage(section->currentPage);
  if (page) {
    const int fontId = SETTINGS.getReaderFontId();
    for (const auto& el : page->elements) {
      if (el->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*el);
      const auto& tb = line.getTextBlock();
      const auto& wordsRef = tb.getWords();
      const auto& xpos = tb.getWordXpos();
      const auto& styles = tb.getWordStyles();
      const int16_t ls = tb.getLetterSpacing();
      for (size_t i = 0; i < wordsRef.size(); i++) {
        crosspoint::reader::WordPos wp;
        wp.x = static_cast<int16_t>(static_cast<int>(xpos[i]) + line.xPos + xOffset);
        wp.y = static_cast<int16_t>(line.yPos + yOffset);
        wp.width = static_cast<int16_t>(renderer.getTextWidthSpaced(fontId, wordsRef[i].c_str(), ls, styles[i]));
        words.push_back(wp);
      }
    }
  }
  highlights_.setWordsForPage(section->currentPage, std::move(words));
}

void EpubReaderActivity::enterHighlightMode() {
  if (!section || section->pageCount == 0) return;
  highlights_.enter();
  requestUpdate();
}

void EpubReaderActivity::exitHighlightMode() {
  highlights_.exit();
  requestUpdate();
}

void EpubReaderActivity::highlightMoveCursor(const int direction) {
  if (!section) return;
  const crosspoint::reader::PageContext ctx{section->currentPage, section->pageCount, highlights_.wordCount()};
  const auto r = highlights_.moveCursor(direction, ctx);
  if (r.pageDelta != 0) section->currentPage += r.pageDelta;
  if (r.stateChanged) requestUpdate();
}

void EpubReaderActivity::highlightMoveCursorLine(const int direction) {
  if (!section) return;
  const crosspoint::reader::PageContext ctx{section->currentPage, section->pageCount, highlights_.wordCount()};
  const auto r = highlights_.moveCursorLine(direction, ctx);
  if (r.pageDelta != 0) section->currentPage += r.pageDelta;
  if (r.stateChanged) requestUpdate();
}

void EpubReaderActivity::highlightConfirmSelection() {
  if (!section) return;
  const auto r = highlights_.confirm(currentSpineIndex, section->currentPage, millis());
  if (r.pageDelta != 0) section->currentPage += r.pageDelta;
  if (r.stateChanged) requestUpdate();
}

void EpubReaderActivity::handleHighlightInput() {
  if (!section) {
    exitHighlightMode();
    return;
  }

  // Back cancels highlight mode
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    exitHighlightMode();
    return;
  }

  // Confirm (release) advances highlight state.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    highlightConfirmSelection();
    return;
  }

  // Up/Down (side buttons) = move cursor up/down by line
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    highlightMoveCursorLine(-1);
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    highlightMoveCursorLine(+1);
    return;
  }

  // Left/PageBack = move cursor backward (previous word)
  if (mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    highlightMoveCursor(-1);
    return;
  }

  // Right/PageForward = move cursor forward (next word)
  if (mappedInput.wasPressed(MappedInputManager::Button::PageForward) ||
      mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    highlightMoveCursor(+1);
    return;
  }
}

void EpubReaderActivity::renderHighlights(const Page& page, const int fontId, const int xOffset, const int yOffset) {
  if (!section) return;
  // Rebuild cache if page changed (uses correct render offsets from renderContents)
  if (!highlights_.wordCacheValidFor(section->currentPage)) {
    rebuildHighlightWordCache(xOffset, yOffset);
  }
  const auto& wordList = highlights_.words();
  if (wordList.empty()) return;

  const int wordCount = static_cast<int>(wordList.size());
  const int textHeight = renderer.getTextHeight(fontId);
  constexpr int thickness = 2;  // SHOW_UNDERLINE dashed underline thickness

  const int cursorIdx = highlights_.cursorIndex() >= wordCount ? wordCount - 1 : highlights_.cursorIndex();

  // Helper: draw cursor as a solid black rectangle with the word text redrawn
  // in white (inverted). Only used during SELECT_START / SELECT_END — the
  // dashed underline appears later during SHOW_UNDERLINE confirmation.
  // `wi` carries text + style for the inverted glyph redraw; `cw` carries the
  // cached geometry used for the black-fill rect.
  const auto drawCursor = [&](const crosspoint::reader::WordPos& cw, const WordInfo* wi) {
    constexpr int pad = 2;  // breathing room between word glyphs and black fill
    const int bx = (cw.x > pad) ? cw.x - pad : 0;
    const int by = (cw.y > pad) ? cw.y - pad : 0;
    const int bw = cw.width + (cw.x - bx) + pad;
    const int bh = textHeight + (cw.y - by) + pad;
    renderer.fillRect(bx, by, bw, bh, true);
    if (wi != nullptr && !wi->text.empty()) {
      renderer.drawTextSpaced(fontId, wi->x, wi->y, wi->text.c_str(), wi->letterSpacing, false, wi->style);
    }
  };

  const auto state = highlights_.state();
  const bool needsCursorText = (state == HighlightState::SELECT_START || state == HighlightState::SELECT_END);
  // buildWordList gives us text + style for white-text redraw on the cursor word.
  std::vector<WordInfo> infoList;
  if (needsCursorText) infoList = buildWordList(page, xOffset, yOffset, fontId);
  const auto wordInfoAt = [&](int idx) -> const WordInfo* {
    if (idx < 0 || idx >= static_cast<int>(infoList.size())) return nullptr;
    return &infoList[idx];
  };

  if (state == HighlightState::SELECT_START) {
    if (cursorIdx >= 0 && cursorIdx < wordCount) {
      drawCursor(wordList[cursorIdx], wordInfoAt(cursorIdx));
    }
  } else if (state == HighlightState::SELECT_END) {
    const int endIdx = highlights_.endWordIndex();
    if (section->currentPage == highlights_.endPage() && endIdx >= 0 && endIdx < wordCount) {
      drawCursor(wordList[endIdx], wordInfoAt(endIdx));
    }
  } else if (state == HighlightState::SHOW_UNDERLINE) {
    const int startPage = highlights_.startPage();
    const int endPage = highlights_.endPage();
    const int startWord = highlights_.startWordIndex();
    const int endWord = highlights_.endWordIndex();
    int selStart = -1;
    int selEnd = -1;

    if (section->currentPage == startPage && section->currentPage == endPage) {
      selStart = startWord;
      selEnd = endWord;
    } else if (section->currentPage == startPage) {
      selStart = startWord;
      selEnd = wordCount - 1;
    } else if (section->currentPage == endPage) {
      selStart = 0;
      selEnd = endWord;
    } else if (section->currentPage > startPage && section->currentPage < endPage) {
      selStart = 0;
      selEnd = wordCount - 1;
    }

    if (selStart >= 0 && selEnd >= 0) {
      if (selStart >= wordCount) selStart = wordCount - 1;
      if (selEnd >= wordCount) selEnd = wordCount - 1;
      // Draw continuous underline per line (no gaps between words)
      int lineY = wordList[selStart].y;
      int lineMinX = wordList[selStart].x;
      int lineMaxX = wordList[selStart].x + wordList[selStart].width;
      for (int i = selStart + 1; i <= selEnd; i++) {
        const auto& w = wordList[i];
        if (w.y != lineY) {
          // Flush previous line
          drawDashedHLine(renderer, lineMinX, lineY + textHeight + 1, lineMaxX - lineMinX, thickness);
          lineY = w.y;
          lineMinX = w.x;
          lineMaxX = w.x + w.width;
        } else {
          if (w.x + w.width > lineMaxX) lineMaxX = w.x + w.width;
        }
      }
      // Flush last line
      drawDashedHLine(renderer, lineMinX, lineY + textHeight + 1, lineMaxX - lineMinX, thickness);
    }
  }
}

std::string EpubReaderActivity::extractQuoteText() {
  const int startPage = highlights_.startPage();
  const int endPage = highlights_.endPage();
  const int startWord = highlights_.startWordIndex();
  const int endWord = highlights_.endWordIndex();
  if (startPage < 0 || endPage < 0 || !section) return "";
  if (startWord < 0 || endWord < 0) return "";

  constexpr size_t kMaxQuoteLength = 8192;
  std::string result;
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  const int contentY = orientedMarginTop;
  const int fontId = SETTINGS.getReaderFontId();

  for (int pg = startPage; pg <= endPage; pg++) {
    auto page = loadAndCachePage(pg);
    if (!page) continue;

    auto wordList = buildWordList(*page, orientedMarginLeft, contentY, fontId);
    if (wordList.empty()) continue;

    int startIdx = (pg == startPage) ? startWord : 0;
    int endIdx = (pg == endPage) ? endWord : static_cast<int>(wordList.size()) - 1;

    // Clamp to word list bounds
    if (startIdx < 0) startIdx = 0;
    if (endIdx >= static_cast<int>(wordList.size())) endIdx = static_cast<int>(wordList.size()) - 1;

    for (int i = startIdx; i <= endIdx; i++) {
      if (!result.empty()) {
        // Check if the word starts with punctuation that should be attached
        const char first = wordList[i].text.empty() ? '\0' : wordList[i].text[0];
        if (first != ',' && first != '.' && first != ';' && first != ':' && first != '!' && first != '?' &&
            first != ')' && first != '"') {
          result += ' ';
        }
      }
      result += wordList[i].text;
      if (result.size() >= kMaxQuoteLength) break;
    }
    if (result.size() >= kMaxQuoteLength) break;
  }

  return result;
}

std::string EpubReaderActivity::getChapterTitle() const {
  if (!epub) return "";
  const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (tocIndex >= 0) {
    const auto tocEntry = epub->getTocItem(tocIndex);
    return tocEntry.title;
  }
  return "Chapter " + std::to_string(currentSpineIndex + 1);
}

std::string EpubReaderActivity::getQuotesFilePath() const {
  if (!epub) return "";
  const std::string bookPath = epub->getPath();
  const auto dotPos = bookPath.rfind('.');
  const std::string basePath = (dotPos != std::string::npos) ? bookPath.substr(0, dotPos) : bookPath;
  return basePath + "_QUOTES.txt";
}

void EpubReaderActivity::saveQuoteToFile(const std::string& quote) {
  if (!epub || quote.empty()) return;

  const std::string quotesPath = getQuotesFilePath();
  const std::string tmpPath = quotesPath + ".tmp";
  const std::string bakPath = quotesPath + ".bak";

  // Atomic read-modify-write: copy existing primary into .tmp, append the new
  // entry, then rotate primary -> .bak and .tmp -> primary. A torn write or
  // power loss leaves .bak as the prior good state.
  if (Storage.exists(tmpPath.c_str())) {
    Storage.remove(tmpPath.c_str());
  }

  HalFile dst;
  if (!Storage.openFileForWrite("HLT", tmpPath, dst)) {
    LOG_ERR("HLT", "Failed to open quotes tmp for writing: %s", tmpPath.c_str());
    return;
  }

  // Copy existing content (if any) into tmp
  if (Storage.exists(quotesPath.c_str())) {
    HalFile src;
    if (Storage.openFileForRead("HLT", quotesPath, src)) {
      uint8_t buffer[512];
      while (src.available()) {
        const int rd = src.read(buffer, sizeof(buffer));
        if (rd <= 0) break;
        if (dst.write(buffer, rd) != static_cast<size_t>(rd)) {
          LOG_ERR("HLT", "Failed to copy existing quotes into tmp");
          src.close();
          dst.close();
          Storage.remove(tmpPath.c_str());
          return;
        }
      }
      src.close();
    }
  }

  // Append new entry
  const std::string chapterTitle = getChapterTitle();
  const std::string entry = "[" + chapterTitle + "]\n" + quote + "\n---\n\n";
  if (dst.write(entry.c_str(), entry.size()) != entry.size()) {
    LOG_ERR("HLT", "Failed to append new quote to tmp");
    dst.close();
    Storage.remove(tmpPath.c_str());
    return;
  }
  dst.flush();
  dst.close();

  // 2-layer rotation
  if (Storage.exists(bakPath.c_str())) {
    if (!Storage.remove(bakPath.c_str())) {
      LOG_ERR("HLT", "Failed to remove stale quotes bak %s", bakPath.c_str());
    }
  }
  if (Storage.exists(quotesPath.c_str())) {
    if (!Storage.rename(quotesPath.c_str(), bakPath.c_str())) {
      LOG_ERR("HLT", "Failed to rotate %s -> %s", quotesPath.c_str(), bakPath.c_str());
      Storage.remove(tmpPath.c_str());
      return;
    }
  }
  if (!Storage.rename(tmpPath.c_str(), quotesPath.c_str())) {
    LOG_ERR("HLT", "Failed to promote quotes tmp to %s", quotesPath.c_str());
    if (Storage.exists(bakPath.c_str())) {
      if (Storage.rename(bakPath.c_str(), quotesPath.c_str())) {
        LOG_INF("HLT", "Restored quotes from .bak after promote failure");
      }
    }
    return;
  }

  LOG_DBG("HLT", "Quote saved to %s", quotesPath.c_str());
}

// ── End Highlight / Quote selection mode ─────────────────────────────────────

bool EpubReaderActivity::renderContents(const Page& page, const int orientedMarginTop, const int orientedMarginRight,
                                        const int orientedMarginBottom, const int orientedMarginLeft,
                                        const StatusBarLayout& statusBarLayout) {
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.setTextRenderStyle(SETTINGS.textRenderMode);

  const int viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  // Vertically center text on full pages so the visible *ink* sits with equal
  // top and bottom gaps. Centering by nominal line boxes leaves the gaps
  // uneven: the font ascender reserves space above caps that a normal first
  // line never fills (extra top gap), and the line-spacing leading sits below
  // the last line (extra bottom gap). We measure the real glyph ink box and
  // center that instead. Only text-only full pages qualify — partial (last)
  // pages stay top-aligned. Trade-off: the shift is content-dependent, so the
  // first/last line ink can nudge the block a few px between pages; on a full
  // e-ink page-turn redraw this is imperceptible.
  int contentY = orientedMarginTop;
  if (page.isTextOnly()) {
    const int fontId = SETTINGS.getReaderFontId();
    const int lineHeight = renderer.getLineHeight(fontId);
    if (lineHeight > 0) {
      const int usedHeight = page.getUsedHeight(lineHeight);
      const int boxSlack = viewportHeight - usedHeight;
      // Full page = at most one line of nominal box slack (dense-fit pages sit
      // at ~0). Partial pages fall through and stay top-aligned.
      if (usedHeight > 0 && boxSlack >= 0 && boxSlack < lineHeight) {
        int inkTop;
        int inkBottom;
        if (page.getInkBounds(renderer, fontId, &inkTop, &inkBottom)) {
          // Shared centering kernel (see ReaderInkCentering.h). Returns 0 for an
          // out-of-range ink box, leaving contentY at the top margin as before.
          contentY = orientedMarginTop + crosspoint::reader::inkCenterOffset(inkTop, inkBottom, viewportHeight);
        }
      }
    }
  }

  // Two-pass font prewarm: scan pass collects text, then decompress needed glyphs.
  // The actual render must happen inside the scope so page buffers stay alive.
  auto* fcm = renderer.getFontCacheManager();
  // Count glyph-bitmap allocation failures that occur during the *actual*
  // render pass only (the scan/prewarm pass below also touches the
  // decompressor but its failures are expected and recoverable). A non-zero
  // count means the heap was too fragmented to decompress some glyph groups,
  // so the frame is partially rendered and would display as scattered-glyph
  // garbage — we discard it and let the caller recover.
  uint32_t glyphOom = 0;
  const uint32_t tFrameStart = millis();
  if (fcm) {
    auto scope = fcm->createPrewarmScope();
    const uint32_t tScanStart = millis();
    page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, contentY);  // scan pass
    const uint32_t tScanEnd = millis();
    // RFC #164 step 7: dial glyph-prewarm warmth from the heap. With comfortable
    // headroom (>=40 KB largest) this resolves to Full -> 0x0F, byte-identical to
    // before; under pressure it warms regular glyphs only, shrinking the
    // simultaneous glyph-cache peak that crowds the render-OOM path.
    const uint8_t prewarmMask = crosspoint::layout::DegradePlan::from(
                                    crosspoint::layout::renderLevelFor(crosspoint::heap::largestFreeBlockBytes(),
                                                                       crosspoint::mem::kRenderTrimPrewarmBelowBytes),
                                    crosspoint::layout::kStyleAll)
                                    .prewarmStyleMask;
    scope.endScanAndPrewarm(prewarmMask);
    auto* fd = fcm->getDecompressor();
    if (fd) fd->resetBitmapAllocFailures();
    const uint32_t tRenderStart = millis();
    page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, contentY);  // actual render
    const uint32_t tRenderEnd = millis();
    if (fd) glyphOom = fd->bitmapAllocFailures();
    LOG_INF("ERS", "page render timings: scan=%u ms render=%u ms total=%u ms",
            static_cast<unsigned>(tScanEnd - tScanStart), static_cast<unsigned>(tRenderEnd - tRenderStart),
            static_cast<unsigned>(tRenderEnd - tFrameStart));
  } else {
    // Uncompressed (built-in) font with no decompressor: glyph bitmaps are
    // direct pointers into flash, so there is no allocation to fail here.
    page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, contentY);
  }
  // Render highlight overlay and border if in highlight/quote selection mode
  if (highlights_.state() != HighlightState::NONE) {
    renderHighlights(page, SETTINGS.getReaderFontId(), orientedMarginLeft, contentY);
    // Frame around the text area marks quote-selection mode. Its look follows the
    // Quote Screen Style setting so the in-book selection matches the saved-quotes
    // viewer: Classic dashed, Terminal solid block.
    constexpr int frameOffset = 6;     // padding from text area to the frame
    constexpr int frameThickness = 5;  // thicker frame for visibility
    const int bx = orientedMarginLeft - frameOffset;
    const int by = contentY - frameOffset;
    const int bw = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight + 2 * frameOffset;
    const int bh = viewportHeight + 2 * frameOffset;
    if (SETTINGS.quoteScreenStyle == CrossPointSettings::QUOTE_STYLE_TERMINAL) {
      for (int t = 0; t < frameThickness; ++t) {
        renderer.drawRect(bx + t, by + t, bw - 2 * t, bh - 2 * t, true);
      }
    } else {
      drawDashedRect(renderer, bx, by, bw, bh, frameThickness);
    }
  }

  if (SETTINGS.debugBorders) {
    DrawUtils::drawDottedRect(renderer, orientedMarginLeft, orientedMarginTop,
                              renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight, viewportHeight);
  }

  renderStatusBar(statusBarLayout, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);

  // Render-time glyph OOM: the frame is incomplete (scattered-glyph garbage).
  // Bail BEFORE displayBuffer() so the broken frame never reaches the panel;
  // the caller routes to silent-restart / recovery. setTextRenderStyle is
  // restored here because the early return skips the reset at the tail.
  if (glyphOom > 0) {
    LOG_ERR("ERS", "render-time glyph OOM (%u alloc failures) — discarding partial frame", (unsigned)glyphOom);
    renderer.setTextRenderStyle(0);
    return false;
  }

  const bool pageHasImages = page.hasImages();

  if (pagesUntilFullRefresh <= 1 || pageHasImages) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  // Differential grayscale overlay for image pages. Text + status bar stay on
  // the BW base above; only the image area gets the 2-bit overlay so other
  // text-only pages still look "normal" via the standard BW differential path.
  if (pageHasImages && renderer.storeBwBuffer()) {
    const Page* pagePtr = &page;
    const int ml = orientedMarginLeft;
    const int cy = contentY;
    auto drawImages = [&, pagePtr, ml, cy]() { pagePtr->renderImages(renderer, ml, cy); };
    renderer.renderGrayscale(GfxRenderer::GrayscaleMode::Differential, drawImages);
    renderer.restoreBwBuffer();
    // Force the next page after an image page to take the HALF_REFRESH branch
    // above, fully clearing any residual grayscale state so text doesn't ghost
    // through. Cheaper than a FULL_REFRESH and visually identical.
    pagesUntilFullRefresh = 1;
  }

  renderer.setTextRenderStyle(0);
  return true;
}

void EpubReaderActivity::renderStatusBar(const StatusBarLayout& statusBarLayout, const int orientedMarginRight,
                                         const int orientedMarginBottom, const int orientedMarginLeft) {
  ReaderStatusBar::renderStatusBar(renderer, statusBarLayout, orientedMarginRight, orientedMarginBottom,
                                   orientedMarginLeft, SETTINGS.debugBorders);
}

void EpubReaderActivity::navigateToHref(const char* href, const bool savePosition) {
  if (!epub || !href) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  std::string hrefStr(href);

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", href);
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  // section.reset() below forces a chapter relayout on the next render — show
  // the same "Loading…" feedback every other reader navigation (chapter skip,
  // percent jump, TOC jump) shows, so footnote follow/return is never silent.
  TransitionFeedback::show(renderer, tr(STR_LOADING));

  {
    RenderLock lock(*this);
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, href);
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}
