/**
 * @file TxtReaderActivity.h
 * @brief Plain-text (.txt) reader with word-wrap and pagination.
 *
 * Loads UTF-8 text files in chunks, reflows to the display width with
 * configurable font/margins, and provides page-based navigation.
 * Shares the status bar and progress tracking infrastructure with
 * EpubReaderActivity via the common ReaderActivity base.
 */
#pragma once

#include <Txt.h>

#include <vector>

#include "CrossPointSettings.h"
#include "PagedProgressSink.h"
#include "ReaderInputDispatcher.h"
#include "ReaderSession.h"
#include "ReaderSessionPorts.h"
#include "ReaderStatusBar.h"
#include "activities/ActivityWithSubactivity.h"

struct RecentBook;

class TxtReaderActivity final : public ActivityWithSubactivity {
  using StatusBarLayout = ReaderStatusBar::StatusBarLayout;
  struct FlowLine {
    std::string text;
    bool firstInParagraph = false;
    bool lastInParagraph = false;
  };

  std::unique_ptr<Txt> txt;

  int currentPage = 0;
  int totalPages = 1;
  int pagesUntilFullRefresh = 0;

  const std::function<void()> onGoBack;
  const std::function<void()> onGoHome;
  const std::function<void(const std::string&)> onOpenBook;
  bool recentSwitcherOpen = false;
  bool pendingSubactivityExit = false;
  // RFC #165: the tap/double-tap/long-press FSM now lives in the shared
  // ReaderInputDispatcher (no doubleTap-vs-menu, no chapter-skip, no footnote
  // for the txt reader). longPressConfirm maps to the orientation toggle.
  crosspoint::reader::ReaderInputDispatcher inputDispatcher_{
      crosspoint::reader::ReaderInputConfig{/*doubleTapToggle=*/true, /*longPressConfirm=*/true,
                                            /*footnoteBack=*/false, /*chapterSkip=*/false}};
  // RFC #171: progress persistence + the onEnter skeleton run through the shared
  // host-tested ReaderSession (composes ReaderProgressTracker). Single-document
  // => ReaderPosition{0, currentPage, 1}; recent-book title is the filename.
  crosspoint::reader::PagedProgressSink progressSink_{"", "TRS"};
  crosspoint::reader::ProdDisplayPort displayPort_{renderer};
  crosspoint::reader::ProdEnvPort envPort_;
  crosspoint::reader::ReaderSession session_{
      {progressSink_, envPort_, displayPort_},
      crosspoint::reader::ReaderHooks{[this] { return txt ? txt->getPath() : std::string(); },
                                      [this] { return crosspoint::reader::ReaderPosition{0, currentPage, 1}; }, nullptr,
                                      nullptr, nullptr,
                                      [this](std::string& title, std::string&, std::string&) {
                                        const std::string p = txt ? txt->getPath() : std::string();
                                        title = p.substr(p.rfind('/') + 1);
                                      }}};
  int recentSwitcherSelection = 0;
  std::vector<RecentBook> recentSwitcherBooks;

  // Streaming text reader - stores file offsets for each page
  std::vector<size_t> pageOffsets;  // File offset for start of each page
  std::vector<FlowLine> currentPageLines;
  int linesPerPage = 0;
  int viewportWidth = 0;
  bool initialized = false;
  int pendingRelayoutPage = -1;
  int pendingRelayoutPageCount = 0;

  // Cached settings for cache validation (different fonts/margins require re-indexing)
  int cachedFontId = 0;
  int cachedScreenMarginHorizontal = 0;
  int cachedScreenMarginTop = 0;
  int cachedScreenMarginBottom = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  uint8_t cachedLineSpacingPercent = 110;
  uint8_t cachedWordSpacingPercent = 100;
  uint8_t cachedFirstLineIndentMode = CrossPointSettings::INDENT_BOOK;
  int cachedTitleUsableWidth = -1;
  bool cachedTitleNoTitleTruncation = false;
  int cachedTitleMaxLines = -1;
  std::vector<std::string> cachedTitleLines;

  void renderPage();
  void renderStatusBar(const StatusBarLayout& statusBarLayout, int orientedMarginRight, int orientedMarginBottom,
                       int orientedMarginLeft);
  void renderRecentSwitcher();
  void openReadingThemes();
  void reloadCurrentLayoutForDisplaySettings();
  int getReaderLineHeightPx() const;
  int getTxtWordSpaceWidth() const;
  int getTxtParagraphIndentPx() const;
  int measureFlowLineWidth(const std::string& text) const;
  void drawFlowLine(const FlowLine& line, int x, int y, int contentWidth) const;
  const std::vector<std::string>& getStatusBarTitleLines(int usableWidth, bool noTitleTruncation,
                                                         int maxTitleLineCount);
  int getStatusBarReserveTitleLineCount(int usableWidth, bool noTitleTruncation);
  StatusBarLayout buildStatusBarLayout(int usableWidth, int topReservedHeight, int bottomReservedHeight,
                                       int maxTitleLineCount);

  void initializeReader();
  bool loadPageAtOffset(size_t offset, std::vector<FlowLine>& outLines, size_t& nextOffset);
  void buildPageIndex();
  bool loadPageIndexCache();
  void savePageIndexCache() const;
  void flushProgressIfNeeded(bool force);
  void loadProgress();
  void toggleTextRenderMode();
  void toggleOrientation();
  // RFC #165: snapshot MappedInputManager into the pure dispatcher frame, then
  // execute the decoded action. The FSM decision is host-tested in the core.
  crosspoint::reader::ReaderInput snapshotInput();
  void applyEffect(const crosspoint::reader::ReaderInputDispatcher::Result& effect);

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt,
                             const std::function<void()>& onGoBack, const std::function<void()>& onGoHome,
                             const std::function<void(const std::string&)>& onOpenBook)
      : ActivityWithSubactivity("TxtReader", renderer, mappedInput),
        txt(std::move(txt)),
        onGoBack(onGoBack),
        onGoHome(onGoHome),
        onOpenBook(onOpenBook) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;
};
