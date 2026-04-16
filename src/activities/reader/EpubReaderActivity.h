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

#include <array>
#include <string>

#include "BookmarkStore.h"
#include "EpubReaderMenuActivity.h"
#include "ReaderStatusBar.h"
#include "activities/ActivityWithSubactivity.h"

class EpubReaderActivity final : public ActivityWithSubactivity {
  // --- Highlight/Quote selection mode ---
  enum class HighlightState { NONE, SELECT_START, SELECT_END, SHOW_UNDERLINE };

  // Lightweight word position info for cursor/underline rendering (no text storage)
  struct WordPos {
    int16_t x;      // screen x position
    int16_t y;      // screen y position
    int16_t width;  // pixel width of the word
  };

  // Full word info including text (only used for quote extraction, not cached)
  struct WordInfo {
    int x;
    int y;
    int width;
    std::string text;
    EpdFontFamily::Style style;
    int16_t letterSpacing;
  };

  HighlightState highlightState = HighlightState::NONE;
  int highlightCursorIndex = 0;      // current cursor position (flat word index on current page)
  int highlightStartSpine = -1;      // spine index where selection started
  int highlightStartPage = -1;       // page number where selection started
  int highlightStartWordIndex = -1;  // flat word index of start on start page
  int highlightEndPage = -1;         // page number of end cursor (may differ from start)
  int highlightEndWordIndex = -1;    // flat word index of end on end page
  unsigned long highlightUnderlineStartMs = 0;  // millis() timestamp when underline display began
  std::vector<WordPos> highlightWordCache;      // cached word positions (6 bytes/word vs ~40 for WordInfo)
  int highlightWordCachePage = -1;              // page index the cache was built for

  std::vector<WordInfo> buildWordList(const Page& page, int xOffset, int yOffset, int fontId) const;
  bool lookupWordInfo(const Page& page, int wordIndex, int xOffset, int yOffset, int fontId, WordInfo& out) const;
  void rebuildHighlightWordCache(int xOffset, int yOffset);  // rebuild cache with correct render offsets
  int highlightWordCount() const;  // word count from cache (0 if empty)
  void enterHighlightMode();
  void exitHighlightMode();
  void highlightMoveCursor(int direction);
  void highlightMoveCursorLine(int direction);
  void highlightConfirmSelection();
  void handleHighlightInput();
  void renderHighlights(const Page& page, int fontId, int xOffset, int yOffset);
  std::string extractQuoteText();
  void saveQuoteToFile(const std::string& quote);
  std::string getChapterTitle() const;
  using StatusBarLayout = ReaderStatusBar::StatusBarLayout;

  struct PageCacheEntry {
    int pageIndex = -1;
    std::shared_ptr<Page> page;
  };

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
  bool pendingMenuOpen = false;
  bool pendingThemeReload = false;      // Defer settings reload after ReadingThemesActivity exits
  unsigned long lastConfirmReleaseMs = 0;
  bool confirmLongPressHandled = false;
  bool progressDirty = false;
  unsigned long lastProgressChangeMs = 0;
  int lastObservedSpineIndex = -1;
  int lastObservedPage = -1;
  int lastObservedPageCount = -1;
  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;
  int pageLoadFailCount = 0;  // Tracks consecutive page load failures to prevent infinite retry loops
  bool pendingSectionReset = false;  // Defer section.reset() from render task to loop (avoids race)
  int cachedReserveSpineIndex = -1;
  int cachedReserveUsableWidth = -1;
  bool cachedReserveNoTitleTruncation = false;
  int cachedReserveTitleLineCount = 1;
  int cachedTitleTocIndex = -2;
  int cachedTitleUsableWidth = -1;
  bool cachedTitleNoTitleTruncation = false;
  int cachedTitleMaxLines = -1;
  std::vector<std::string> cachedTitleLines;
  int pageCacheSpineIndex = -1;
  std::array<PageCacheEntry, 3> pageCache;
  const std::function<void()> onGoBack;
  const std::function<void()> onGoHome;
  const std::function<void(const std::string&)> onOpenBook;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  void renderContents(const Page& page, int orientedMarginTop, int orientedMarginRight, int orientedMarginBottom,
                      int orientedMarginLeft, const StatusBarLayout& statusBarLayout);
  void renderStatusBar(const StatusBarLayout& statusBarLayout, int orientedMarginRight, int orientedMarginBottom,
                       int orientedMarginLeft);
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  void saveProgress(int spineIndex, int currentPage, int pageCount);
  void flushProgressIfNeeded(bool force);
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
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuBack(uint8_t orientation);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void applyOrientation(uint8_t orientation);
  void reloadCurrentSectionForDisplaySettings();
  void loopSubActivity();
  void loopHighlightMode();
  void loopPageTurn(bool prevTriggered, bool nextTriggered);
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
