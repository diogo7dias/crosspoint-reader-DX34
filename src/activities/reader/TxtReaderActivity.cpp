#include "TxtReaderActivity.h"

#include <EpdFontFamily.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Serialization.h>
#include <Utf8.h>
#include <algorithm>
#include <esp_task_wdt.h>
#include <memory>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ReadingThemesActivity.h"
#include "ReaderLayoutSafety.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StatusPopup.h"
#include "util/StringUtils.h"
#include "util/TransitionFeedback.h"

namespace {
constexpr unsigned long goHomeMs = 1000;
constexpr unsigned long confirmDoubleTapMs = 350;
constexpr unsigned long progressSaveDebounceMs = 800;
constexpr int progressBarMarginTop = 1;
constexpr int recentSwitcherRows = 8;
constexpr size_t CHUNK_SIZE = 8 * 1024; // 8KB chunk for reading
// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449; // "TXTI"
constexpr uint8_t CACHE_VERSION = 4; // Increment when cache format changes

uint8_t normalizeTxtParagraphAlignment(const uint8_t alignment) {
  return alignment == CrossPointSettings::BOOK_STYLE
             ? (uint8_t)CrossPointSettings::JUSTIFIED
             : alignment;
}

int countBreakableSpaces(const std::string& text) {
  return static_cast<int>(std::count(text.begin(), text.end(), ' '));
}

using ReaderStatusBar::computeStatusBarsHeight;
using ReaderStatusBar::computeStatusBarReservedHeight;
using ReaderStatusBar::computeStatusTextBlockHeight;
using ReaderStatusBar::getStatusBottomInset;
using ReaderStatusBar::getStatusTopInset;
using ReaderStatusBar::normalizeReaderMargins;
using ReaderStatusBar::statusBarItemIsTop;
using ReaderStatusBar::statusTextPositionIsTop;

std::string formatPageCounterText(const uint8_t mode, const int currentPage,
                                  const int totalPages) {
  const int safeTotalPages = std::max(totalPages, 0);
  const int safeCurrentPage = std::max(currentPage, 0);
  int pagesLeft = safeTotalPages - (currentPage + 1);
  if (pagesLeft < 0) {
    pagesLeft = 0;
  }

  switch (mode) {
    case CrossPointSettings::STATUS_PAGE_LEFT_TEXT:
      return std::to_string(pagesLeft) + " left";
    default:
      return std::to_string(safeCurrentPage + 1) + "/" +
             std::to_string(safeTotalPages);
  }
}

} // namespace

void TxtReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (!txt) {
    return;
  }

  // Configure screen orientation based on settings
  switch (SETTINGS.orientation) {
  case CrossPointSettings::ORIENTATION::PORTRAIT:
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
    break;
  case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
    renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
    break;
  case CrossPointSettings::ORIENTATION::INVERTED:
    renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
    break;
  case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
    renderer.setOrientation(
        GfxRenderer::Orientation::LandscapeCounterClockwise);
    break;
  default:
    break;
  }
  EpdFontFamily::setReaderBoldSwapEnabled(SETTINGS.readerBoldSwap != 0);

  txt->setupCacheDir();

  // Save current txt as last opened file and add to recent books
  auto filePath = txt->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  // Move book to /recents/ folder on first open from another location
  {
    std::string newPath = RECENT_BOOKS.moveBookToRecents(txt->getPath());
    if (!newPath.empty()) {
      txt->setPath(newPath);
      filePath = newPath;
      fileName = filePath.substr(filePath.rfind('/') + 1);
      APP_STATE.openEpubPath = filePath;
      APP_STATE.saveToFile();
    }
  }

  cachedTitleUsableWidth = -1;
  cachedTitleNoTitleTruncation = false;
  cachedTitleMaxLines = -1;
  cachedTitleLines.clear();

  // Trigger first update
  requestUpdate();
}

void TxtReaderActivity::onExit() {
  flushProgressIfNeeded(true);
  EpdFontFamily::setReaderBoldSwapEnabled(false);
  ActivityWithSubactivity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  pageOffsets.clear();
  currentPageLines.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  cachedTitleUsableWidth = -1;
  cachedTitleNoTitleTruncation = false;
  cachedTitleMaxLines = -1;
  cachedTitleLines.clear();
  txt.reset();
}

const std::vector<std::string>& TxtReaderActivity::getStatusBarTitleLines(
    const int usableWidth, const bool noTitleTruncation,
    const int maxTitleLineCount) {
  if (cachedTitleUsableWidth == usableWidth &&
      cachedTitleNoTitleTruncation == noTitleTruncation &&
      cachedTitleMaxLines == maxTitleLineCount) {
    return cachedTitleLines;
  }

  std::string titleText = txt ? txt->getTitle() : "";
  if (titleText.empty()) {
    titleText = tr(STR_UNNAMED);
  }

  cachedTitleLines = ReaderLayoutSafety::buildTitleLines(
      renderer, SETTINGS.getStatusBarFontId(), titleText, usableWidth, noTitleTruncation,
      maxTitleLineCount);

  cachedTitleUsableWidth = usableWidth;
  cachedTitleNoTitleTruncation = noTitleTruncation;
  cachedTitleMaxLines = maxTitleLineCount;
  return cachedTitleLines;
}

int TxtReaderActivity::getStatusBarReserveTitleLineCount(
    const int usableWidth, const bool noTitleTruncation) {
  return static_cast<int>(
      getStatusBarTitleLines(usableWidth, noTitleTruncation, 1024).size());
}

TxtReaderActivity::StatusBarLayout TxtReaderActivity::buildStatusBarLayout(
    const int usableWidth, const int topReservedHeight,
    const int bottomReservedHeight, const int maxTitleLineCount) {
  StatusBarLayout layout;
  layout.usableWidth = ReaderLayoutSafety::clampViewportDimension(
      usableWidth, ReaderLayoutSafety::kMinViewportWidth, "TRS",
      "status width");
  layout.topReservedHeight = topReservedHeight;
  layout.bottomReservedHeight = bottomReservedHeight;
  if (!SETTINGS.statusBarEnabled) {
    return layout;
  }

  const float progress =
      totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0.0f;
  layout.bookProgress = progress;
  layout.chapterProgress = progress;

  if (SETTINGS.statusBarShowPageCounter) {
    layout.pageCounterText = formatPageCounterText(
        SETTINGS.statusBarPageCounterMode, currentPage, totalPages);
    layout.pageCounterTextWidth =
        renderer.getTextWidth(SETTINGS.getStatusBarFontId(), layout.pageCounterText.c_str());
  }
  if (SETTINGS.statusBarShowBookPercentage) {
    char bookPercentageStr[16] = {0};
    snprintf(bookPercentageStr, sizeof(bookPercentageStr), "B:%.0f%%",
             layout.bookProgress);
    layout.bookPercentageText = bookPercentageStr;
    layout.bookPercentageTextWidth = renderer.getTextWidth(
        SETTINGS.getStatusBarFontId(), layout.bookPercentageText.c_str());
  }
  if (SETTINGS.statusBarShowChapterPercentage) {
    char chapterPercentageStr[16] = {0};
    snprintf(chapterPercentageStr, sizeof(chapterPercentageStr), "C:%.0f%%",
             layout.chapterProgress);
    layout.chapterPercentageText = chapterPercentageStr;
    layout.chapterPercentageTextWidth = renderer.getTextWidth(
        SETTINGS.getStatusBarFontId(), layout.chapterPercentageText.c_str());
  }

  if (SETTINGS.statusBarShowBookPageCounter && totalPages > 0) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d/%d", currentPage + 1, totalPages);
    layout.bookPageCounterText = buf;
    layout.bookPageCounterTextWidth =
        renderer.getTextWidth(SETTINGS.getStatusBarFontId(), buf);
  }

  if (SETTINGS.statusBarShowChapterTitle) {
    constexpr int titlePadding = 4;
    const int titleWrapWidth = renderer.getScreenWidth() - titlePadding * 2;
    layout.titleLines = getStatusBarTitleLines(
        titleWrapWidth, SETTINGS.statusBarNoTitleTruncation,
        maxTitleLineCount);
  }

  return layout;
}

int TxtReaderActivity::getReaderLineHeightPx() const {
  const int baseLineHeight = renderer.getLineHeight(cachedFontId);
  return std::max(1, (baseLineHeight * static_cast<int>(cachedLineSpacingPercent)) /
                         100);
}

int TxtReaderActivity::getTxtWordSpaceWidth() const {
  const int base = renderer.getSpaceWidth(cachedFontId);
  return std::max(
      1, base + CrossPointSettings::wordSpacingSettingToPixelDelta(
                    cachedWordSpacingPercent, base));
}

int TxtReaderActivity::getTxtParagraphIndentPx() const {
  switch (cachedFirstLineIndentMode) {
    case CrossPointSettings::INDENT_OFF:
      return 0;
    case CrossPointSettings::INDENT_SMALL:
      return (renderer.getTextAdvanceX(cachedFontId, "\xe2\x80\x83") * 6) / 10;
    case CrossPointSettings::INDENT_LARGE:
      return (renderer.getTextAdvanceX(cachedFontId, "\xe2\x80\x83") * 14) / 10;
    case CrossPointSettings::INDENT_BOOK:
    case CrossPointSettings::INDENT_MEDIUM:
    default:
      return renderer.getTextAdvanceX(cachedFontId, "\xe2\x80\x83");
  }
}

int TxtReaderActivity::measureFlowLineWidth(const std::string& text) const {
  const int baseWidth = renderer.getTextWidth(cachedFontId, text.c_str());
  const int baseSpaceWidth = renderer.getSpaceWidth(cachedFontId);
  return baseWidth + countBreakableSpaces(text) *
                         (getTxtWordSpaceWidth() - baseSpaceWidth);
}

void TxtReaderActivity::drawFlowLine(const FlowLine& line, const int x,
                                     const int y, const int contentWidth) const {
  if (line.text.empty()) {
    return;
  }

  const bool allowIndent =
      cachedParagraphAlignment == CrossPointSettings::LEFT_ALIGN ||
      cachedParagraphAlignment == CrossPointSettings::JUSTIFIED;
  const int indent =
      line.firstInParagraph && allowIndent ? getTxtParagraphIndentPx() : 0;
  const int baseSpaceWidth = getTxtWordSpaceWidth();
  const int lineWidth = measureFlowLineWidth(line.text);
  int drawX = x + indent;
  int spacingWidth = baseSpaceWidth;
  int actualGapCount = countBreakableSpaces(line.text);

  if (cachedParagraphAlignment == CrossPointSettings::CENTER_ALIGN) {
    drawX = x + (contentWidth - lineWidth) / 2;
  } else if (cachedParagraphAlignment == CrossPointSettings::RIGHT_ALIGN) {
    drawX = x + contentWidth - lineWidth;
  } else if (cachedParagraphAlignment == CrossPointSettings::JUSTIFIED &&
             !line.lastInParagraph && actualGapCount > 0) {
    const int nonSpaceWidth = lineWidth - actualGapCount * baseSpaceWidth;
    const int effectiveWidth = contentWidth - indent;
    const int spare = effectiveWidth - nonSpaceWidth;
    spacingWidth = std::max(0, spare / actualGapCount);
  }

  size_t pos = 0;
  while (pos < line.text.size()) {
    const size_t spacePos = line.text.find(' ', pos);
    const size_t tokenLen = (spacePos == std::string::npos)
                                ? (line.text.size() - pos)
                                : (spacePos - pos);
    if (tokenLen > 0) {
      const std::string token = line.text.substr(pos, tokenLen);
      renderer.drawText(cachedFontId, drawX, y, token.c_str());
      drawX += renderer.getTextWidth(cachedFontId, token.c_str());
    }

    if (spacePos == std::string::npos) {
      break;
    }

    size_t nextPos = spacePos;
    while (nextPos < line.text.size() && line.text[nextPos] == ' ') {
      drawX += spacingWidth;
      nextPos++;
    }
    pos = nextPos;
  }
}

void TxtReaderActivity::openReadingThemes() {
  exitActivity();
  enterNewActivity(new ReadingThemesActivity(
      renderer, mappedInput, txt ? txt->getCachePath() : std::string(),
      [this](const bool changed) {
        pendingSubactivityExit = true;
        if (changed) {
          reloadCurrentLayoutForDisplaySettings();
        } else {
          requestUpdate();
        }
      }));
}

void TxtReaderActivity::reloadCurrentLayoutForDisplaySettings() {
  TransitionFeedback::show(renderer, tr(STR_LOADING));
  flushProgressIfNeeded(true);
  pendingRelayoutPage = currentPage;
  pendingRelayoutPageCount = std::max(totalPages, 1);
  lastSavedPage = currentPage;
  lastObservedPage = currentPage;
  progressDirty = false;
  cachedTitleUsableWidth = -1;
  cachedTitleNoTitleTruncation = false;
  cachedTitleMaxLines = -1;
  cachedTitleLines.clear();
  initialized = false;
  pageOffsets.clear();
  currentPageLines.clear();
}

void TxtReaderActivity::loop() {
  flushProgressIfNeeded(false);

  if (subActivity) {
    subActivity->loop();
    if (pendingSubactivityExit) {
      pendingSubactivityExit = false;
      exitActivity();  // suppressUntilAllReleased() called inside
      requestUpdate();
    }
    return;
  }

  if (pendingThemesOpen &&
      !mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      millis() - lastConfirmReleaseMs > confirmDoubleTapMs) {
    pendingThemesOpen = false;
    openReadingThemes();
    return;
  }

  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    confirmLongPressHandled = false;
  }

  if (recentSwitcherOpen) {
    const bool prevTriggered =
        mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool nextTriggered =
        mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right);
    if (prevTriggered && !recentSwitcherBooks.empty()) {
      recentSwitcherSelection =
          (recentSwitcherSelection +
           static_cast<int>(recentSwitcherBooks.size()) - 1) %
          recentSwitcherBooks.size();
      requestUpdate();
      return;
    }
    if (nextTriggered && !recentSwitcherBooks.empty()) {
      recentSwitcherSelection =
          (recentSwitcherSelection + 1) % recentSwitcherBooks.size();
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) &&
        !recentSwitcherBooks.empty()) {
      const std::string selectedPath =
          recentSwitcherBooks[recentSwitcherSelection].path;
      recentSwitcherOpen = false;
      if (!selectedPath.empty()) {
        onOpenBook(selectedPath);
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
        mappedInput.getHeldTime() < goHomeMs) {
      recentSwitcherOpen = false;
      requestUpdate();
      return;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (mappedInput.getHeldTime() >= goHomeMs) {
      return;
    }
    const unsigned long now = millis();
    if (pendingThemesOpen &&
        now - lastConfirmReleaseMs <= confirmDoubleTapMs) {
      pendingThemesOpen = false;
      toggleTextRenderMode();
      return;
    }
    pendingThemesOpen = true;
    lastConfirmReleaseMs = now;
    return;
  }

  // Long press CONFIRM (1s+) toggles orientation: Portrait <-> Landscape CCW.
  if (!confirmLongPressHandled &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= goHomeMs) {
    confirmLongPressHandled = true;
    mappedInput.suppressUntilAllReleased();
    pendingThemesOpen = false;
    SETTINGS.orientation =
        (SETTINGS.orientation == CrossPointSettings::ORIENTATION::LANDSCAPE_CCW)
            ? CrossPointSettings::ORIENTATION::PORTRAIT
            : CrossPointSettings::ORIENTATION::LANDSCAPE_CCW;
    if (!SETTINGS.saveToFile()) {
      LOG_ERR("TRS", "Failed to save settings after orientation change");
    }
    renderer.setOrientation(
        SETTINGS.orientation == CrossPointSettings::ORIENTATION::LANDSCAPE_CCW
            ? GfxRenderer::Orientation::LandscapeCounterClockwise
            : GfxRenderer::Orientation::Portrait);
    reloadCurrentLayoutForDisplaySettings();
    requestUpdate();
    return;
  }

  // BACK: go home immediately on press for snappier response.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  // When long-press chapter skip is disabled, turn pages on press instead of
  // release.
  const bool usePressForPageTurn = !SETTINGS.longPressChapterSkip;
  const bool prevTriggered =
      usePressForPageTurn
          ? (mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
             mappedInput.wasPressed(MappedInputManager::Button::Left))
          : (mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
             mappedInput.wasReleased(MappedInputManager::Button::Left));
  const bool powerPageTurn =
      SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
      mappedInput.wasReleased(MappedInputManager::Button::Power);
  const bool nextTriggered =
      usePressForPageTurn
          ? (mappedInput.wasPressed(MappedInputManager::Button::PageForward) ||
             powerPageTurn ||
             mappedInput.wasPressed(MappedInputManager::Button::Right))
          : (mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
             powerPageTurn ||
             mappedInput.wasReleased(MappedInputManager::Button::Right));

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (prevTriggered && currentPage > 0) {
    currentPage--;
    progressDirty = true;
    lastProgressChangeMs = millis();
    flushProgressIfNeeded(true);
    requestUpdate();
  } else if (nextTriggered) {
    if (currentPage < totalPages - 1) {
      currentPage++;
      APP_STATE.sessionPagesRead++;
      progressDirty = true;
      lastProgressChangeMs = millis();
      flushProgressIfNeeded(true);
      requestUpdate();
    } else {
      onGoHome();
    }
  }
}

void TxtReaderActivity::toggleTextRenderMode() {
  flushProgressIfNeeded(true);
  SETTINGS.textRenderMode =
      (SETTINGS.textRenderMode + 1) % CrossPointSettings::TEXT_RENDER_MODE_COUNT;
  if (!SETTINGS.saveToFile()) {
    LOG_ERR("TRS", "Failed to save settings after text render mode toggle");
  }

  if (txt) {
    Storage.remove((txt->getCachePath() + "/index.bin").c_str());
  }
  reloadCurrentLayoutForDisplaySettings();
  requestUpdate();
}

void TxtReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  // Store current settings for cache validation
  cachedFontId = SETTINGS.getReaderFontId();
  if (SETTINGS.dynamicMargins) {
    // Auto-calculate horizontal margins to target ~62 characters per line.
    // Use normalized oriented margins (same as EpubReaderActivity) so both
    // readers produce identical horizontal margins for the same font/screen.
    const int sampleWidth = renderer.getTextWidth(cachedFontId, "abcdefghijklmnopqrstuvwxyz");
    const int avgCharWidth = (sampleWidth > 0) ? sampleWidth / 26 : 8;
    constexpr int targetCPL = 62;
    const int targetTextWidth = targetCPL * avgCharWidth;
    int baseTop, baseRight, baseBottom, baseLeft;
    renderer.getOrientedViewableTRBL(&baseTop, &baseRight, &baseBottom, &baseLeft);
    normalizeReaderMargins(&baseTop, &baseRight, &baseBottom, &baseLeft);
    const int availableWidth = renderer.getScreenWidth() - baseLeft - baseRight;
    cachedScreenMarginHorizontal = std::max(0, std::min(55, (availableWidth - targetTextWidth) / 2));
  } else {
    cachedScreenMarginHorizontal = SETTINGS.screenMarginHorizontal;
  }
  cachedScreenMarginTop = SETTINGS.screenMarginTop;
  cachedScreenMarginBottom = SETTINGS.screenMarginBottom;
  cachedParagraphAlignment =
      normalizeTxtParagraphAlignment(SETTINGS.paragraphAlignment);
  cachedLineSpacingPercent = SETTINGS.lineSpacingPercent;
  cachedWordSpacingPercent = SETTINGS.wordSpacingPercent;
  cachedFirstLineIndentMode = SETTINGS.firstLineIndentMode;
  const int lineHeight = getReaderLineHeightPx();
  const int minContentHeight = std::max(ReaderLayoutSafety::kMinViewportHeight,
                                        lineHeight * 2);

  // Calculate viewport dimensions
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom,
      orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight,
                                   &orientedMarginBottom, &orientedMarginLeft);
  normalizeReaderMargins(&orientedMarginTop, &orientedMarginRight,
                         &orientedMarginBottom, &orientedMarginLeft);
  orientedMarginTop += cachedScreenMarginTop;
  orientedMarginLeft += cachedScreenMarginHorizontal;
  orientedMarginRight += cachedScreenMarginHorizontal;
  orientedMarginBottom += cachedScreenMarginBottom;

  if (SETTINGS.statusBarEnabled) {
    const int usableWidth =
        ReaderLayoutSafety::clampViewportDimension(
            renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight,
            ReaderLayoutSafety::kMinViewportWidth, "TRS", "usable width");
    const bool showTopStatusTextRow =
        (SETTINGS.statusBarShowBattery &&
         statusTextPositionIsTop(SETTINGS.statusBarBatteryPosition)) ||
        (SETTINGS.statusBarShowPageCounter &&
         statusTextPositionIsTop(SETTINGS.statusBarPageCounterPosition)) ||
        (SETTINGS.statusBarShowBookPercentage &&
         statusTextPositionIsTop(SETTINGS.statusBarBookPercentagePosition)) ||
        (SETTINGS.statusBarShowChapterPercentage &&
         statusTextPositionIsTop(
             SETTINGS.statusBarChapterPercentagePosition)) ||
        (SETTINGS.statusBarShowBookPageCounter &&
         statusTextPositionIsTop(SETTINGS.statusBarBookPageCounterPosition));
    const bool showBottomStatusTextRow =
        (SETTINGS.statusBarShowBattery &&
         !statusTextPositionIsTop(SETTINGS.statusBarBatteryPosition)) ||
        (SETTINGS.statusBarShowPageCounter &&
         !statusTextPositionIsTop(SETTINGS.statusBarPageCounterPosition)) ||
        (SETTINGS.statusBarShowBookPercentage &&
         !statusTextPositionIsTop(SETTINGS.statusBarBookPercentagePosition)) ||
        (SETTINGS.statusBarShowChapterPercentage &&
         !statusTextPositionIsTop(
             SETTINGS.statusBarChapterPercentagePosition)) ||
        (SETTINGS.statusBarShowBookPageCounter &&
         !statusTextPositionIsTop(SETTINGS.statusBarBookPageCounterPosition));
    const int titleLineCount =
        SETTINGS.statusBarShowChapterTitle
            ? (SETTINGS.statusBarNoTitleTruncation
                   ? getStatusBarReserveTitleLineCount(
                         usableWidth, SETTINGS.statusBarNoTitleTruncation)
                   : 1)
            : 0;
    const int topTitleLineCount =
        (SETTINGS.statusBarShowChapterTitle &&
         statusBarItemIsTop(SETTINGS.statusBarTitlePosition))
            ? titleLineCount
            : 0;
    const int bottomTitleLineCount =
        (SETTINGS.statusBarShowChapterTitle &&
         !statusBarItemIsTop(SETTINGS.statusBarTitlePosition))
            ? titleLineCount
            : 0;
    const auto budget = ReaderLayoutSafety::resolveStatusBarBudget(
        renderer, SETTINGS.getStatusBarFontId(), "TRS", renderer.getScreenHeight(), getStatusTopInset(renderer),
        getStatusBottomInset(renderer), cachedScreenMarginTop,
        cachedScreenMarginBottom, minContentHeight,
        SETTINGS.getStatusBarProgressBarHeight(),
        ReaderLayoutSafety::StatusBarBandConfig{
            .showStatusTextRow = showTopStatusTextRow,
            .showBookProgressBar =
                SETTINGS.statusBarShowBookBar &&
                statusBarItemIsTop(SETTINGS.statusBarBookBarPosition),
            .showChapterProgressBar =
                SETTINGS.statusBarShowChapterBar &&
                statusBarItemIsTop(SETTINGS.statusBarChapterBarPosition),
            .desiredTitleLineCount = topTitleLineCount,
        },
        ReaderLayoutSafety::StatusBarBandConfig{
            .showStatusTextRow = showBottomStatusTextRow,
            .showBookProgressBar =
                SETTINGS.statusBarShowBookBar &&
                !statusBarItemIsTop(SETTINGS.statusBarBookBarPosition),
            .showChapterProgressBar =
                SETTINGS.statusBarShowChapterBar &&
                !statusBarItemIsTop(SETTINGS.statusBarChapterBarPosition),
            .desiredTitleLineCount = bottomTitleLineCount,
        });
    const int statusBarTopReserved = budget.top.reservedHeight;
    const int statusBarBottomReserved = budget.bottom.reservedHeight;
    if (statusBarTopReserved > 0) {
      orientedMarginTop =
          getStatusTopInset(renderer) + cachedScreenMarginTop +
          statusBarTopReserved;
    }
    if (statusBarBottomReserved > 0) {
      orientedMarginBottom =
          getStatusBottomInset(renderer) + cachedScreenMarginBottom +
          statusBarBottomReserved;
    }
  }

  viewportWidth =
      ReaderLayoutSafety::clampViewportDimension(
          renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight,
          ReaderLayoutSafety::kMinViewportWidth, "TRS", "viewport width");
  const int viewportHeight =
      ReaderLayoutSafety::clampViewportDimension(
          renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom,
          minContentHeight, "TRS", "viewport height");

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1)
    linesPerPage = 1;

  LOG_DBG("TRS", "Viewport: %dx%d, lines per page: %d", viewportWidth,
          viewportHeight, linesPerPage);

  // Try to load cached page index first
  if (!loadPageIndexCache()) {
    // Cache not found, build page index
    buildPageIndex();
    // Save to cache for next time
    savePageIndexCache();
  }

  // Load saved progress
  loadProgress();
  if (pendingRelayoutPageCount > 0 && totalPages > 0) {
    if (pendingRelayoutPageCount != totalPages) {
      const float progress =
          pendingRelayoutPageCount > 1
              ? static_cast<float>(pendingRelayoutPage) /
                    static_cast<float>(pendingRelayoutPageCount - 1)
              : 0.0f;
      currentPage = static_cast<int>(
          progress * static_cast<float>(std::max(totalPages - 1, 0)));
    } else {
      currentPage = std::min(pendingRelayoutPage, totalPages - 1);
    }
    currentPage = std::max(0, std::min(currentPage, totalPages - 1));
    lastSavedPage = currentPage;
    lastObservedPage = currentPage;
    pendingRelayoutPage = -1;
    pendingRelayoutPageCount = 0;
    saveProgress();
  }

  initialized = true;
}

void TxtReaderActivity::buildPageIndex() {
  pageOffsets.clear();
  pageOffsets.push_back(0); // First page starts at offset 0

  size_t offset = 0;
  const size_t fileSize = txt->getFileSize();

  LOG_DBG("TRS", "Building page index for %zu bytes...", fileSize);

  StatusPopup::showBlocking(renderer, tr(STR_INDEXING));

  constexpr size_t MAX_PAGE_OFFSETS = 5000;
  while (offset < fileSize) {
    std::vector<FlowLine> tempLines;
    size_t nextOffset = offset;

    if (!loadPageAtOffset(offset, tempLines, nextOffset)) {
      break;
    }

    if (nextOffset <= offset) {
      // No progress made, avoid infinite loop
      break;
    }

    offset = nextOffset;
    if (offset < fileSize) {
      pageOffsets.push_back(offset);
    }

    if (pageOffsets.size() >= MAX_PAGE_OFFSETS) {
      LOG_INF("TRS", "Page index capped at %zu pages to conserve memory",
              MAX_PAGE_OFFSETS);
      break;
    }

    // Yield to other tasks periodically and reset watchdog
    if (pageOffsets.size() % 20 == 0) {
      esp_task_wdt_reset();
      vTaskDelay(1);
    }
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Built page index: %d pages", totalPages);
}

bool TxtReaderActivity::loadPageAtOffset(size_t offset,
                                         std::vector<FlowLine> &outLines,
                                         size_t &nextOffset) {
  outLines.clear();
  const size_t fileSize = txt->getFileSize();

  if (offset >= fileSize) {
    return false;
  }

  // Read a chunk from file — scale down if heap is tight
  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  const size_t freeHeap = esp_get_free_heap_size();
  if (freeHeap < chunkSize * 3) {
    // Heap is tight — use a smaller chunk to leave room for word-wrap strings
    chunkSize = std::min(chunkSize, std::max(static_cast<size_t>(1024), freeHeap / 4));
    LOG_DBG("TRS", "Reduced chunk to %zu bytes (free heap: %zu)", chunkSize, freeHeap);
  }
  auto buffer = std::unique_ptr<uint8_t[]>(new(std::nothrow) uint8_t[chunkSize + 1]);
  if (!buffer) {
    LOG_ERR("TRS", "Failed to allocate %zu bytes (free heap: %zu)", chunkSize, freeHeap);
    return false;
  }

  if (!txt->readContent(buffer.get(), offset, chunkSize)) {
    return false;
  }
  buffer[chunkSize] = '\0';

  // Parse lines from buffer
  size_t pos = 0;

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') {
      lineEnd++;
    }

    // Check if we have a complete line
    bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= fileSize);

    if (!lineComplete && static_cast<int>(outLines.size()) > 0) {
      // Incomplete line and we already have some lines, stop here
      break;
    }

    // Calculate the actual length of line content in the buffer (excluding
    // newline)
    size_t lineContentLen = lineEnd - pos;

    // Check for carriage return
    bool hasCR =
        (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;

    // Extract line content for display (without CR/LF)
    std::string line(reinterpret_cast<char *>(buffer.get() + pos), displayLen);

    // Track position within this source line (in bytes from pos)
    size_t lineBytePos = 0;

    // Word wrap if needed
    bool firstSegmentInParagraph = true;
    while (!line.empty() && static_cast<int>(outLines.size()) < linesPerPage) {
      const bool allowIndent =
          cachedParagraphAlignment == CrossPointSettings::LEFT_ALIGN ||
          cachedParagraphAlignment == CrossPointSettings::JUSTIFIED;
      const int indentWidth =
          (firstSegmentInParagraph && allowIndent) ? getTxtParagraphIndentPx()
                                                   : 0;
      const int availableWidth =
          std::max(1, viewportWidth - indentWidth);
      int lineWidth = measureFlowLineWidth(line);

      if (lineWidth <= availableWidth) {
        outLines.push_back(FlowLine{.text = line,
                                    .firstInParagraph = firstSegmentInParagraph,
                                    .lastInParagraph = true});
        lineBytePos = displayLen; // Consumed entire display content
        line.clear();
        break;
      }

      // Find break point — try spaces first (fast), then character-by-character fallback
      size_t breakPos = line.length();
      {
        // First pass: scan forward through spaces, stop once we exceed width
        size_t lastGoodSpace = 0;
        size_t spaceSearch = 0;
        while ((spaceSearch = line.find(' ', spaceSearch)) != std::string::npos) {
          if (spaceSearch > 0) {
            if (measureFlowLineWidth(line.substr(0, spaceSearch)) <= availableWidth) {
              lastGoodSpace = spaceSearch;
            } else {
              break;  // this space overflows, all later ones will too
            }
          }
          spaceSearch++;
        }
        if (lastGoodSpace > 0) {
          breakPos = lastGoodSpace;
        } else {
          // No space fits — fall back to character-by-character search
          while (breakPos > 0 &&
                 measureFlowLineWidth(line.substr(0, breakPos)) >
                     availableWidth) {
            breakPos--;
            // Make sure we don't break in the middle of a UTF-8 sequence
            while (breakPos > 0 && (line[breakPos] & 0xC0) == 0x80) {
              breakPos--;
            }
          }
        }
      }

      if (breakPos == 0) {
        breakPos = 1;
      }

      outLines.push_back(FlowLine{.text = line.substr(0, breakPos),
                                  .firstInParagraph = firstSegmentInParagraph,
                                  .lastInParagraph = false});
      firstSegmentInParagraph = false;

      // Skip space at break point
      size_t skipChars = breakPos;
      if (breakPos < line.length() && line[breakPos] == ' ') {
        skipChars++;
      }
      lineBytePos += skipChars;
      line = line.substr(skipChars);
    }

    // Determine how much of the source buffer we consumed
    if (line.empty()) {
      // Fully consumed this source line, move past the newline
      pos = lineEnd + 1;
    } else {
      // Partially consumed - page is full mid-line
      // Move pos to where we stopped in the line (NOT past the line)
      pos = pos + lineBytePos;
      break;
    }
  }

  // Ensure we make progress even if calculations go wrong
  if (pos == 0 && !outLines.empty()) {
    // Fallback: at minimum, consume something to avoid infinite loop
    pos = 1;
  }

  nextOffset = offset + pos;

  // Make sure we don't go past the file
  if (nextOffset > fileSize) {
    nextOffset = fileSize;
  }

  return !outLines.empty();
}

void TxtReaderActivity::render(Activity::RenderLock &&) {
  if (!txt) {
    return;
  }

  if (recentSwitcherOpen) {
    renderRecentSwitcher();
    return;
  }

  // Initialize reader if not done
  if (!initialized) {
    initializeReader();
    TransitionFeedback::dismiss(renderer);
  }

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true,
                              EpdFontFamily::REGULAR);
    renderer.displayBuffer();
    return;
  }

  // Bounds check
  if (currentPage < 0)
    currentPage = 0;
  if (currentPage >= totalPages)
    currentPage = totalPages - 1;

  // Load current page content
  size_t offset = pageOffsets[currentPage];
  size_t nextOffset;
  currentPageLines.clear();
  if (!loadPageAtOffset(offset, currentPageLines, nextOffset) || currentPageLines.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true,
                              EpdFontFamily::REGULAR);
    renderer.displayBuffer();
    return;
  }

  renderer.clearScreen();
  renderPage();

  if (lastObservedPage != currentPage) {
    lastObservedPage = currentPage;
    if (lastSavedPage != currentPage) {
      progressDirty = true;
      lastProgressChangeMs = millis();
    }
  }

  flushProgressIfNeeded(false);
}

void TxtReaderActivity::renderRecentSwitcher() {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int popupX = 18;
  const int popupY = 24;
  const int popupW = screenW - popupX * 2;
  const int popupH = screenH - popupY * 2;
  const int titleY = popupY + 8;
  const int rowsY = popupY + 30;
  const int rowsH = popupH - 40;
  const int rowH = rowsH / recentSwitcherRows;

  renderer.clearScreen();
  renderer.drawRect(popupX, popupY, popupW, popupH, true);
  renderer.drawCenteredText(UI_12_FONT_ID, titleY, tr(STR_MENU_RECENT_BOOKS),
                            true, EpdFontFamily::REGULAR);

  for (int i = 0; i < recentSwitcherRows; i++) {
    const int rowY = rowsY + i * rowH;
    const bool hasBook = i < static_cast<int>(recentSwitcherBooks.size());
    const bool selected = hasBook && i == recentSwitcherSelection;

    if (selected) {
      renderer.fillRect(popupX + 8, rowY, popupW - 16, rowH - 2, true);
      renderer.drawRect(popupX + 10, rowY + 2, popupW - 20, rowH - 6, false);
    } else {
      renderer.drawRect(popupX + 8, rowY, popupW - 16, rowH - 2, true);
    }

    std::string title = " ";
    if (hasBook) {
      title = recentSwitcherBooks[i].title;
      if (title.empty()) {
        const size_t lastSlash = recentSwitcherBooks[i].path.find_last_of('/');
        title = (lastSlash == std::string::npos)
                    ? recentSwitcherBooks[i].path
                    : recentSwitcherBooks[i].path.substr(lastSlash + 1);
      }
      title = renderer.truncatedText(UI_10_FONT_ID, title.c_str(), popupW - 28);
    }
    renderer.drawText(UI_10_FONT_ID, popupX + 14, rowY + 3, title.c_str(),
                      !selected);
  }

  renderer.displayBuffer();
}

void TxtReaderActivity::renderPage() {
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom,
      orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight,
                                   &orientedMarginBottom, &orientedMarginLeft);
  normalizeReaderMargins(&orientedMarginTop, &orientedMarginRight,
                         &orientedMarginBottom, &orientedMarginLeft);
  orientedMarginTop += cachedScreenMarginTop;
  orientedMarginLeft += cachedScreenMarginHorizontal;
  orientedMarginRight += cachedScreenMarginHorizontal;
  orientedMarginBottom += cachedScreenMarginBottom;

  const int lineHeight = getReaderLineHeightPx();
  const int minContentHeight = std::max(ReaderLayoutSafety::kMinViewportHeight,
                                        lineHeight * 2);
  const int contentWidth = viewportWidth;
  const int usableWidth =
      ReaderLayoutSafety::clampViewportDimension(
          renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight,
          ReaderLayoutSafety::kMinViewportWidth, "TRS", "usable width");
  int statusBarTopReserved = 0;
  int statusBarBottomReserved = 0;
  int resolvedTitleLineCount = SETTINGS.statusBarShowChapterTitle ? 1 : 0;
  if (SETTINGS.statusBarEnabled) {
    const bool showTopStatusTextRow =
        (SETTINGS.statusBarShowBattery &&
         statusTextPositionIsTop(SETTINGS.statusBarBatteryPosition)) ||
        (SETTINGS.statusBarShowPageCounter &&
         statusTextPositionIsTop(SETTINGS.statusBarPageCounterPosition)) ||
        (SETTINGS.statusBarShowBookPercentage &&
         statusTextPositionIsTop(SETTINGS.statusBarBookPercentagePosition)) ||
        (SETTINGS.statusBarShowChapterPercentage &&
         statusTextPositionIsTop(
             SETTINGS.statusBarChapterPercentagePosition)) ||
        (SETTINGS.statusBarShowBookPageCounter &&
         statusTextPositionIsTop(SETTINGS.statusBarBookPageCounterPosition));
    const bool showBottomStatusTextRow =
        (SETTINGS.statusBarShowBattery &&
         !statusTextPositionIsTop(SETTINGS.statusBarBatteryPosition)) ||
        (SETTINGS.statusBarShowPageCounter &&
         !statusTextPositionIsTop(SETTINGS.statusBarPageCounterPosition)) ||
        (SETTINGS.statusBarShowBookPercentage &&
         !statusTextPositionIsTop(SETTINGS.statusBarBookPercentagePosition)) ||
        (SETTINGS.statusBarShowChapterPercentage &&
         !statusTextPositionIsTop(
             SETTINGS.statusBarChapterPercentagePosition)) ||
        (SETTINGS.statusBarShowBookPageCounter &&
         !statusTextPositionIsTop(SETTINGS.statusBarBookPageCounterPosition));
    const int titleLineCount =
        SETTINGS.statusBarShowChapterTitle
            ? (SETTINGS.statusBarNoTitleTruncation
                   ? getStatusBarReserveTitleLineCount(
                         usableWidth, SETTINGS.statusBarNoTitleTruncation)
                   : 1)
            : 0;
    const int topTitleLineCount =
        (SETTINGS.statusBarShowChapterTitle &&
         statusBarItemIsTop(SETTINGS.statusBarTitlePosition))
            ? titleLineCount
            : 0;
    const int bottomTitleLineCount =
        (SETTINGS.statusBarShowChapterTitle &&
         !statusBarItemIsTop(SETTINGS.statusBarTitlePosition))
            ? titleLineCount
            : 0;
    const auto budget = ReaderLayoutSafety::resolveStatusBarBudget(
        renderer, SETTINGS.getStatusBarFontId(), "TRS", renderer.getScreenHeight(), getStatusTopInset(renderer),
        getStatusBottomInset(renderer), cachedScreenMarginTop,
        cachedScreenMarginBottom, minContentHeight,
        SETTINGS.getStatusBarProgressBarHeight(),
        ReaderLayoutSafety::StatusBarBandConfig{
            .showStatusTextRow = showTopStatusTextRow,
            .showBookProgressBar =
                SETTINGS.statusBarShowBookBar &&
                statusBarItemIsTop(SETTINGS.statusBarBookBarPosition),
            .showChapterProgressBar =
                SETTINGS.statusBarShowChapterBar &&
                statusBarItemIsTop(SETTINGS.statusBarChapterBarPosition),
            .desiredTitleLineCount = topTitleLineCount,
        },
        ReaderLayoutSafety::StatusBarBandConfig{
            .showStatusTextRow = showBottomStatusTextRow,
            .showBookProgressBar =
                SETTINGS.statusBarShowBookBar &&
                !statusBarItemIsTop(SETTINGS.statusBarBookBarPosition),
            .showChapterProgressBar =
                SETTINGS.statusBarShowChapterBar &&
                !statusBarItemIsTop(SETTINGS.statusBarChapterBarPosition),
            .desiredTitleLineCount = bottomTitleLineCount,
        });
    statusBarTopReserved = budget.top.reservedHeight;
    statusBarBottomReserved = budget.bottom.reservedHeight;
    resolvedTitleLineCount =
        statusBarItemIsTop(SETTINGS.statusBarTitlePosition)
            ? budget.top.titleLineCount
            : budget.bottom.titleLineCount;
    if (statusBarTopReserved > 0) {
      orientedMarginTop =
          getStatusTopInset(renderer) + cachedScreenMarginTop +
          statusBarTopReserved;
    }
    if (statusBarBottomReserved > 0) {
      orientedMarginBottom =
          getStatusBottomInset(renderer) + cachedScreenMarginBottom +
          statusBarBottomReserved;
    }
  }
  const StatusBarLayout statusBarLayout =
      buildStatusBarLayout(usableWidth, statusBarTopReserved,
                           statusBarBottomReserved, resolvedTitleLineCount);
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.setTextRenderStyle(SETTINGS.textRenderMode);

  // Render text lines with alignment
  auto renderLines = [&]() {
    int y = orientedMarginTop;
    for (const auto &line : currentPageLines) {
      if (!line.text.empty()) {
        drawFlowLine(line, orientedMarginLeft, y, contentWidth);
      }
      y += lineHeight;
    }
  };

  // Two-pass font prewarm: scan pass collects text, then decompress needed glyphs.
  // The actual render must happen inside the scope so page buffers stay alive.
  auto* fcm = renderer.getFontCacheManager();
  if (fcm) {
    auto scope = fcm->createPrewarmScope();
    renderLines();  // scan pass
    scope.endScanAndPrewarm();
    renderLines();  // actual render (BW)
  } else {
    renderLines();
  }
  renderStatusBar(statusBarLayout, orientedMarginRight, orientedMarginBottom,
                  orientedMarginLeft);

  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  renderer.setTextRenderStyle(0);
}

void TxtReaderActivity::renderStatusBar(const StatusBarLayout& statusBarLayout,
                                        const int orientedMarginRight,
                                        const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  ReaderStatusBar::renderStatusBar(renderer, statusBarLayout, orientedMarginRight,
                                   orientedMarginBottom, orientedMarginLeft, false);
}

void TxtReaderActivity::saveProgress() const {
  const std::string progPath = txt->getCachePath() + "/progress.bin";
  const std::string tmpPath = txt->getCachePath() + "/progress_tmp.bin";

  if (Storage.exists(tmpPath.c_str())) {
    Storage.remove(tmpPath.c_str());
  }

  FsFile f;
  if (Storage.openFileForWrite("TRS", tmpPath.c_str(), f)) {
    uint8_t data[4];
    const uint32_t page =
        currentPage < 0 ? 0u : static_cast<uint32_t>(currentPage);
    data[0] = page & 0xFF;
    data[1] = (page >> 8) & 0xFF;
    data[2] = (page >> 16) & 0xFF;
    data[3] = (page >> 24) & 0xFF;
    f.write(data, 4);
    f.close();

    if (Storage.exists(progPath.c_str())) {
      Storage.remove(progPath.c_str());
    }
    Storage.rename(tmpPath.c_str(), progPath.c_str());
  }
}

void TxtReaderActivity::flushProgressIfNeeded(const bool force) {
  if (!txt || !progressDirty) {
    return;
  }

  const auto now = millis();
  if (!force && now - lastProgressChangeMs < progressSaveDebounceMs) {
    return;
  }

  saveProgress();
  lastSavedPage = currentPage;
  progressDirty = false;
}

void TxtReaderActivity::loadProgress() {
  FsFile f;
  if (Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin",
                              f)) {
    uint8_t data[4];
    const int bytesRead = f.read(data, sizeof(data));
    if (bytesRead >= 2) {
      if (bytesRead >= 4) {
        currentPage = static_cast<int>(static_cast<uint32_t>(data[0]) |
                                       (static_cast<uint32_t>(data[1]) << 8) |
                                       (static_cast<uint32_t>(data[2]) << 16) |
                                       (static_cast<uint32_t>(data[3]) << 24));
      } else {
        // Backward compatibility with older 2-byte progress files.
        currentPage = data[0] + (data[1] << 8);
      }
      if (currentPage >= totalPages) {
        currentPage = totalPages - 1;
      }
      if (currentPage < 0) {
        currentPage = 0;
      }
      LOG_DBG("TRS", "Loaded progress: page %d/%d", currentPage, totalPages);
      lastSavedPage = currentPage;
      lastObservedPage = currentPage;
      progressDirty = false;
    }
    f.close();
  }
}

bool TxtReaderActivity::loadPageIndexCache() {
  // Cache file format (using serialization module):
  // - uint32_t: magic "TXTI"
  // - uint8_t: cache version
  // - uint32_t: file size (to validate cache)
  // - int32_t: viewport width
  // - int32_t: lines per page
  // - int32_t: font ID (to invalidate cache on font change)
  // - int32_t: horizontal/top/bottom margins (to invalidate cache on margin
  // changes)
  // - uint8_t: paragraph alignment (to invalidate cache on alignment change)
  // - uint8_t: line spacing percent
  // - uint8_t: word spacing level
  // - uint8_t: first-line indent mode
  // - uint32_t: total pages count
  // - N * uint32_t: page offsets

  std::string cachePath = txt->getCachePath() + "/index.bin";
  FsFile f;
  if (!Storage.openFileForRead("TRS", cachePath, f)) {
    LOG_DBG("TRS", "No page index cache found");
    return false;
  }

  // Read and validate header using serialization module
  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) {
    LOG_DBG("TRS", "Cache magic mismatch, rebuilding");
    f.close();
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("TRS", "Cache version mismatch (%d != %d), rebuilding", version,
            CACHE_VERSION);
    f.close();
    return false;
  }

  uint32_t fileSize;
  serialization::readPod(f, fileSize);
  if (fileSize != txt->getFileSize()) {
    LOG_DBG("TRS", "Cache file size mismatch, rebuilding");
    f.close();
    return false;
  }

  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) {
    LOG_DBG("TRS", "Cache viewport width mismatch, rebuilding");
    f.close();
    return false;
  }

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) {
    LOG_DBG("TRS", "Cache lines per page mismatch, rebuilding");
    f.close();
    return false;
  }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) {
    LOG_DBG("TRS", "Cache font ID mismatch (%d != %d), rebuilding", fontId,
            cachedFontId);
    f.close();
    return false;
  }

  int32_t marginHorizontal;
  serialization::readPod(f, marginHorizontal);
  int32_t marginTop;
  serialization::readPod(f, marginTop);
  int32_t marginBottom;
  serialization::readPod(f, marginBottom);
  if (marginHorizontal != cachedScreenMarginHorizontal ||
      marginTop != cachedScreenMarginTop ||
      marginBottom != cachedScreenMarginBottom) {
    LOG_DBG("TRS", "Cache screen margins mismatch, rebuilding");
    f.close();
    return false;
  }

  uint8_t alignment;
  serialization::readPod(f, alignment);
  if (alignment != cachedParagraphAlignment) {
    LOG_DBG("TRS", "Cache paragraph alignment mismatch, rebuilding");
    f.close();
    return false;
  }

  uint8_t lineSpacingPercent;
  serialization::readPod(f, lineSpacingPercent);
  uint8_t wordSpacingPercent;
  serialization::readPod(f, wordSpacingPercent);
  uint8_t firstLineIndentMode;
  serialization::readPod(f, firstLineIndentMode);
  if (lineSpacingPercent != cachedLineSpacingPercent ||
      wordSpacingPercent != cachedWordSpacingPercent ||
      firstLineIndentMode != cachedFirstLineIndentMode) {
    LOG_DBG("TRS", "Cache spacing settings mismatch, rebuilding");
    f.close();
    return false;
  }

  uint32_t numPages;
  serialization::readPod(f, numPages);

  // Read page offsets
  pageOffsets.clear();
  pageOffsets.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset;
    serialization::readPod(f, offset);
    pageOffsets.push_back(offset);
  }

  f.close();
  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Loaded page index cache: %d pages", totalPages);
  return true;
}

void TxtReaderActivity::savePageIndexCache() const {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  FsFile f;
  if (!Storage.openFileForWrite("TRS", cachePath, f)) {
    LOG_ERR("TRS", "Failed to save page index cache");
    return;
  }

  // Write header using serialization module
  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f,
                          static_cast<int32_t>(cachedScreenMarginHorizontal));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMarginTop));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMarginBottom));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, cachedLineSpacingPercent);
  serialization::writePod(f, cachedWordSpacingPercent);
  serialization::writePod(f, cachedFirstLineIndentMode);
  serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));

  // Write page offsets
  for (size_t offset : pageOffsets) {
    serialization::writePod(f, static_cast<uint32_t>(offset));
  }

  f.close();
  LOG_DBG("TRS", "Saved page index cache: %d pages", totalPages);
}
