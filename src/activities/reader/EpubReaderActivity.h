/**
 * @file EpubReaderActivity.h
 * @brief Core EPUB reader — text reflow, pagination, and rendering.
 *
 * Parses EPUB 2/3 archives, reflows HTML content to fit the e-ink display,
 * handles chapter navigation, bookmarks, text search, footnotes, and
 * reading progress tracking. Supports embedded fonts (via EpdFont),
 * inline images (JPEG/PNG), and KOReader sync.
 */
#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>
#include <Epub/layout/LayoutArena.h>

#include <array>
#include <memory>
#include <string>

#include "BookmarkStore.h"
#include "EpubProgressSink.h"
#include "EpubReaderMenuActivity.h"
#include "HighlightController.h"
#include "ReaderInputDispatcher.h"
#include "ReaderProgressTracker.h"
#include "ReaderStatusBar.h"
#include "SectionPageCache.h"
#include "activities/ActivityWithSubactivity.h"

class EpubReaderActivity final : public ActivityWithSubactivity {
  // --- Highlight/Quote selection mode ---
  // State machine + cursor/word-cache lives in HighlightController. This class
  // retains only the rendering + quote-extraction bits that touch GfxRenderer
  // / Page / Section (not host-testable).
  using HighlightState = crosspoint::reader::HighlightController::State;

  // Full word info including text (only used for quote extraction, not cached
  // — HighlightController holds the 6-byte WordPos cache for cursor rendering).
  struct WordInfo {
    int x;
    int y;
    int width;
    std::string text;
    EpdFontFamily::Style style;
    int16_t letterSpacing;
  };

  crosspoint::reader::HighlightController highlights_;

  std::vector<WordInfo> buildWordList(const Page& page, int xOffset, int yOffset, int fontId) const;
  void rebuildHighlightWordCache(int xOffset, int yOffset);  // rebuild cache with correct render offsets
  void enterHighlightMode();
  void exitHighlightMode();
  void highlightMoveCursor(int direction);
  void highlightMoveCursorLine(int direction);
  void highlightConfirmSelection();
  void handleHighlightInput();
  void renderHighlights(const Page& page, int fontId, int xOffset, int yOffset);
  std::string extractQuoteText();
  void saveQuoteToFile(const std::string& quote);
  std::string getQuotesFilePath() const;
  std::string getChapterTitle() const;
  using StatusBarLayout = ReaderStatusBar::StatusBarLayout;

  crosspoint::reader::SectionPageCache<Page> cache_;

  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  std::string pendingAnchor;
  bool pendingSubactivityExit = false;  // Defer subactivity exit to avoid use-after-free
  bool pendingGoHome = false;           // Defer go home to avoid race condition with display task
  bool pendingGoLibrary = false;        // Defer go library after destructive actions
  bool pendingThemeReload = false;      // Defer settings reload after ReadingThemesActivity exits
  // Snapshot of the per-book Bold Swap preference at the moment the reader
  // menu was opened. Compared on menu exit; if the user toggled the in-menu
  // Bold Swap item, the current page is re-laid out to apply the change.
  bool boldSwapAtMenuOpen = false;
  // Input decode FSM (tap/double-tap/long-press/page-turn). The pending-tap,
  // double-tap-window and long-press latches that used to be loose members
  // (pendingMenuOpen / lastConfirmReleaseMs / confirmLongPressHandled) now live
  // inside the dispatcher. Epub opts into all gestures.
  crosspoint::reader::ReaderInputDispatcher inputDispatcher_{
      crosspoint::reader::ReaderInputConfig{/*doubleTapToggle=*/true, /*longPressConfirm=*/true,
                                            /*footnoteBack=*/true, /*chapterSkip=*/true}};
  crosspoint::reader::EpubProgressSink progressSink_{"", 0};
  crosspoint::reader::ReaderProgressTracker progress_{progressSink_};
  int pageLoadFailCount = 0;  // Tracks consecutive page load failures to prevent infinite retry loops

  // Layout arena, doubling as the heap reservation anchor (RFC #164 step 6).
  // Allocated once at onEnter while the heap is still fresh, so its contiguous
  // `kLayoutHeapAnchorBytes` block is guaranteed to exist. The parser lays the
  // section's bounded word buffer + DP scratch into it (passed down through
  // Section::createSectionFile), so it is no longer dead reserve — but it keeps
  // the anchor role too: the pre-flight gate's ReleaseAnchor rung frees it to
  // hand the retry contiguous space (the layout then runs on the std::vector
  // fallback in that freed space). After a successful build we best-effort
  // tryReacquire. Released at onExit. See heapHeadroomOkForLayout and
  // ensureSectionLoaded for the lifecycle.
  crosspoint::layout::LayoutArena layoutHeapAnchor_;
  // Best-effort re-create. Silent no-op when the largest contiguous block is too
  // close to the arena size to leave headroom for the next section build.
  void tryReacquireLayoutHeapAnchor();
  // Cross-task handoff: render runs on the display task, but tearing down `section` must happen on
  // the loop task — its destructor closes file handles and frees page-layout storage that the
  // render task may still be iterating. Setting this flag asks the loop to call section.reset()
  // at a safe point. The direct section.reset() calls in onExit / restoreSavedPosition / goto-href
  // are only safe because those paths are already running on the loop task under a RenderLock.
  bool pendingSectionReset = false;
  // While true, normal reader input (page turns, menu) is suppressed and
  // the next button release re-enters layout via a pendingSectionReset.
  // Set when ensureSectionLoaded paints the recoverable error screen
  // (pre-flight low-memory abort or post-revert "switched to default font"
  // notice). See EpubReaderActivity::loop for the dispatch.
  enum class LayoutRecoveryState : uint8_t {
    None = 0,
    AwaitingRetryAfterRevert,  // we already reverted to default font; tap re-enters
    AwaitingRetryNoRevert,     // pre-flight gate fired; tap will run defrag and retry without revert
  };
  LayoutRecoveryState layoutRecoveryState_ = LayoutRecoveryState::None;
  // Set when an open is abandoned after the silent-restart budget is spent
  // (heap exhausted, not just fragmented). Tells onExit() to PRESERVE the
  // durable boot crash-loop guard (readerActivityLoadCount) instead of
  // resetting it, so the next boot force-routes to the library rather than
  // reopening this same un-openable book. See giveUpOpenToHome().
  bool openGiveUpExit_ = false;
  // Memoized status-bar title wrap results. Two sub-caches with distinct keys:
  //   - reserve: max TOC-title wrap height for the current spine (input to budget resolution).
  //   - titleLines: wrapped lines for the currently-displayed title (input to the paint step).
  // Both are invalidated together via clear() from invalidateStatusBarCaches() — bundling
  // forces every invalidation point through the same method so we can't accidentally reset
  // one sub-cache but leave the other live (a bug we hit during the status-bar V2 work).
  // Field naming preserves the original loose-field spelling on purpose: minimizes diff noise
  // at read/write sites and keeps "grep cachedReserveSpineIndex" working.
  struct StatusBarTitleCache {
    int cachedReserveSpineIndex = -1;
    int cachedReserveUsableWidth = -1;
    bool cachedReserveNoTitleTruncation = false;
    int cachedReserveTitleLineCount = 1;
    int cachedTitleTocIndex = -2;
    int cachedTitleUsableWidth = -1;
    bool cachedTitleNoTitleTruncation = false;
    int cachedTitleMaxLines = -1;
    std::vector<std::string> cachedTitleLines;

    void clear() {
      cachedReserveSpineIndex = -1;
      cachedReserveUsableWidth = -1;
      cachedReserveNoTitleTruncation = false;
      cachedReserveTitleLineCount = 1;
      cachedTitleTocIndex = -2;
      cachedTitleUsableWidth = -1;
      cachedTitleNoTitleTruncation = false;
      cachedTitleMaxLines = -1;
      cachedTitleLines.clear();
    }
  };
  StatusBarTitleCache statusBarCache_;
  const std::function<void()> onGoBack;
  const std::function<void()> onGoHome;
  const std::function<void(const std::string&)> onOpenBook;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  // Footnote back-stack uses a fixed-size array, not std::vector: the UX caps nested footnotes
  // at depth 3 (deeper jumps collapse to a flat navigation), and fixed storage keeps per-activity
  // RAM constant — critical on a device with ~180 KB free heap where fragmentation during long
  // reading sessions has bitten us before. Pop order: decrement `footnoteDepth` FIRST, then index
  // `savedPositions[footnoteDepth]` — the slot at the new depth is the frame we're returning to,
  // not the one we just left (see restoreSavedPosition in the .cpp).
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // Returns false when render-time glyph allocations failed on a fragmented
  // heap — the partial (scattered-glyph) frame is NOT displayed and the caller
  // routes to silent-restart / recovery. Returns true on a complete render.
  bool renderContents(const Page& page, int orientedMarginTop, int orientedMarginRight, int orientedMarginBottom,
                      int orientedMarginLeft, const StatusBarLayout& statusBarLayout);
  void renderStatusBar(const StatusBarLayout& statusBarLayout, int orientedMarginRight, int orientedMarginBottom,
                       int orientedMarginLeft);
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  void saveProgress(int spineIndex, int currentPage, int pageCount);
  void flushProgressIfNeeded(bool force);
  // Synchronously persist the last-good position right before a recovery
  // reboot (silentRestartToReader reboots without flushing, and page-turn
  // writes are deferred to lifecycle events).
  void persistProgressBeforeRestart();
  void invalidateStatusBarCaches();
  void clearPageCache();
  std::shared_ptr<Page> getCachedPage(int pageIndex) const;
  std::shared_ptr<Page> loadAndCachePage(int pageIndex);
  void refreshPageCacheWindow(int centerPage, const std::shared_ptr<Page>& currentPage);
  int getWrappedStatusBarReserveLineCount(int usableWidth);
  const std::vector<std::string>& getStatusBarTitleLines(int tocIndex, int usableWidth, bool noTitleTruncation,
                                                         int maxTitleLineCount);
  StatusBarLayout buildStatusBarLayout(int usableWidth, int topReservedHeight, int bottomReservedHeight,
                                       int maxTitleLineCount);
  // Writes StatusBarLayout::bookPageCounterText + width when the setting is enabled AND we have
  // enough chapter-size data to extrapolate. Left empty otherwise (the renderer treats empty as
  // "don't draw"). Isolated from buildStatusBarLayout because the extrapolation math (21 lines,
  // pages-per-byte × total book size) is the densest block in the status-bar pipeline and the
  // only one with non-trivial WHY notes (chapter density variance).
  void populateBookPageCounterText(StatusBarLayout& layout) const;
  // If `section` is already live, no-op. Otherwise constructs a Section for the current spine
  // index, loads (or builds-then-loads) its page-layout cache, frees transient memory used only
  // during layout (font glyph caches + CSS parser) to reclaim heap for the render pass, and
  // applies any pending cross-section navigation (percent jump / anchor / cachedChapterTotalPageCount
  // reconciliation) to `section->currentPage`.
  // Returns false when the caller should bail out of the current render — either OOM during
  // construction, or createSectionFile failed to persist layout to SD. On false the caller has
  // already had an error dialog painted by this function and should just return.
  bool ensureSectionLoaded(uint16_t viewportWidth, uint16_t viewportHeight);
  // Free every releasable cache that competes with the EPUB-layout heap
  // budget: in-RAM page cache, CSS rules dictionary, built-in font glyph
  // cache, status-bar title caches. Run before retrying createSectionFile
  // when a fragmentation-driven OOM drops layout, and from the pre-flight
  // gate when the largest free block is below the safety threshold.
  void releaseMaxResources();
  // Returns true if the heap looks healthy enough to attempt layout, false
  // if the pre-flight gate ran a defrag pass and is still below the hard
  // floor (caller should paint the low-memory error screen with a retry
  // button instead of attempting createSectionFile).
  bool heapHeadroomOkForLayout();
  // Helpers for the two recovery error screens. Both paint a centered
  // two-line message + retry hint and set layoutRecoveryState_ so the
  // next button release re-enters layout.
  void showLayoutRecoveryScreen(LayoutRecoveryState newState);
  // Terminal recovery for an open that cannot be satisfied: the heap is
  // exhausted and the silent-restart budget is spent, so retrying is futile.
  // Paint a brief low-memory notice, preserve the boot crash-loop guard, and
  // bail to the library instead of stranding the user on a retry-only screen.
  void giveUpOpenToHome();
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuBack(uint8_t orientation);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void applyOrientation(uint8_t orientation);
  void reloadCurrentSectionForDisplaySettings();
  void loopSubActivity();
  void loopHighlightMode();
  // Build the per-frame input snapshot from MappedInputManager for the dispatcher.
  crosspoint::reader::ReaderInput snapshotInput();
  // Execute one decoded reader action (the impure half of the old loopPageTurn
  // + the menu/highlight/back actions): RenderLock, section work, navigation.
  void applyEffect(const crosspoint::reader::ReaderInputDispatcher::Result& effect);
  void openReaderMenu();
  void toggleTextRenderMode();
  void addSessionPagesRead(uint32_t amount = 1);

  // Footnote navigation
  void navigateToHref(const char* href, bool savePosition = false);
  void restoreSavedPosition();

  // Bookmarks
  BookmarkStore bookmarkStore;

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub,
                              const std::function<void()>& onGoBack, const std::function<void()>& onGoHome,
                              const std::function<void(const std::string&)>& onOpenBook)
      : ActivityWithSubactivity("EpubReader", renderer, mappedInput),
        epub(std::move(epub)),
        onGoBack(onGoBack),
        onGoHome(onGoHome),
        onOpenBook(onOpenBook) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&& lock) override;
};
