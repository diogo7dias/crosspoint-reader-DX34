#include "ReaderStatusBar.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "ReaderLayoutSafety.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DrawUtils.h"

namespace ReaderStatusBar {

void drawStyledProgressBar(const GfxRenderer& renderer, const size_t progressPercent, const int y, const int height) {
  int vieweableMarginTop, vieweableMarginRight, vieweableMarginBottom, vieweableMarginLeft;
  renderer.getOrientedViewableTRBL(&vieweableMarginTop, &vieweableMarginRight, &vieweableMarginBottom,
                                   &vieweableMarginLeft);
  const int maxWidth = renderer.getScreenWidth() - vieweableMarginLeft - vieweableMarginRight;
  const int startX = vieweableMarginLeft;
  // At 100%, extend past the right viewable margin to the screen edge
  const int barWidth = (progressPercent >= 100)
      ? (renderer.getScreenWidth() - startX)
      : (maxWidth * static_cast<int>(progressPercent) / 100);
  renderer.fillRect(startX, y, barWidth, height, true);
}

void normalizeReaderMargins(int* top, int* right, int* bottom, int* left) {
  const int vertical = std::max(*top, *bottom);
  const int horizontal = std::max(*left, *right);
  *top = vertical;
  *bottom = vertical;
  *left = horizontal;
  *right = horizontal;
}

int getStatusBottomInset(const GfxRenderer& renderer) {
  int baseTop, baseRight, baseBottom, baseLeft;
  renderer.getOrientedViewableTRBL(&baseTop, &baseRight, &baseBottom, &baseLeft);
  return baseBottom;
}

int getStatusTopInset(const GfxRenderer& renderer) {
  int baseTop, baseRight, baseBottom, baseLeft;
  renderer.getOrientedViewableTRBL(&baseTop, &baseRight, &baseBottom, &baseLeft);
  return baseTop;
}

bool statusBarItemIsTop(const uint8_t position) { return position == CrossPointSettings::STATUS_AT_TOP; }

bool statusTextPositionIsTop(const uint8_t position) { return position <= CrossPointSettings::STATUS_TEXT_TOP_RIGHT; }

int statusTextPositionHorizontalSlot(const uint8_t position) {
  switch (position) {
    case CrossPointSettings::STATUS_TEXT_TOP_LEFT:
    case CrossPointSettings::STATUS_TEXT_BOTTOM_LEFT:
      return 0;
    case CrossPointSettings::STATUS_TEXT_TOP_RIGHT:
    case CrossPointSettings::STATUS_TEXT_BOTTOM_RIGHT:
      return 2;
    case CrossPointSettings::STATUS_TEXT_TOP_CENTER:
    case CrossPointSettings::STATUS_TEXT_BOTTOM_CENTER:
    default:
      return 1;
  }
}

int computeStatusTextBlockHeight(const GfxRenderer& renderer, const int fontId, const bool showStatusTextRow, const int titleLineCount) {
  return ReaderLayoutSafety::computeStatusTextBlockHeight(renderer, fontId, showStatusTextRow, titleLineCount);
}

int computeStatusBarsHeight(const bool showBookProgressBar, const bool showChapterProgressBar,
                            const int statusBarProgressHeight, const bool includeTopMargin) {
  return ReaderLayoutSafety::computeStatusBarsHeight(showBookProgressBar, showChapterProgressBar,
                                                     statusBarProgressHeight, includeTopMargin);
}

std::vector<std::string> wrapStatusText(const GfxRenderer& renderer, const int fontId, const std::string& text,
                                        const int maxWidth) {
  return ReaderLayoutSafety::wrapText(renderer, fontId, text, maxWidth);
}

int computeStatusBarReservedHeight(const GfxRenderer& renderer, const int fontId,
                                   const bool showStatusTextRow,
                                   const bool showBookProgressBar, const bool showChapterProgressBar,
                                   const int titleLineCount) {
  return ReaderLayoutSafety::computeReservedHeight(renderer, fontId, showStatusTextRow, showBookProgressBar,
                                                   showChapterProgressBar, titleLineCount,
                                                   SETTINGS.getStatusBarProgressBarHeight());
}

void renderStatusBar(GfxRenderer& renderer, const StatusBarLayout& statusBarLayout,
                     const int orientedMarginRight, const int orientedMarginBottom,
                     const int orientedMarginLeft, const bool debugBorders) {
  auto metrics = UITheme::getInstance().getMetrics();
  (void)orientedMarginRight;
  (void)orientedMarginBottom;

  if (!SETTINGS.statusBarEnabled) {
    return;
  }
  const bool showBattery = SETTINGS.statusBarShowBattery;
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;
  const int statusFontId = SETTINGS.getStatusBarFontId();
  const auto screenHeight = renderer.getScreenHeight();
  const int usableWidth = statusBarLayout.usableWidth;
  const int statusTopInset = getStatusTopInset(renderer);
  const int statusBottomInset = getStatusBottomInset(renderer);
  const int textHeight = renderer.getTextHeight(statusFontId);
  const int progressBarHeight = SETTINGS.getStatusBarProgressBarHeight();

  const auto renderBand = [&](const int bandTopY, const int reservedHeight, const bool renderTopBand) {
    if (reservedHeight <= 0) {
      return;
    }

    const bool showBandBattery = showBattery && showBatteryPercentage &&
                                 (statusTextPositionIsTop(SETTINGS.statusBarBatteryPosition) == renderTopBand);
    const bool showBandPageCounter = !statusBarLayout.pageCounterText.empty() &&
                                     (statusTextPositionIsTop(SETTINGS.statusBarPageCounterPosition) == renderTopBand);
    const bool showBandBookPercentage =
        !statusBarLayout.bookPercentageText.empty() &&
        (statusTextPositionIsTop(SETTINGS.statusBarBookPercentagePosition) == renderTopBand);
    const bool showBandChapterPercentage =
        !statusBarLayout.chapterPercentageText.empty() &&
        (statusTextPositionIsTop(SETTINGS.statusBarChapterPercentagePosition) == renderTopBand);
    const bool showBandBookPageCounter =
        !statusBarLayout.bookPageCounterText.empty() &&
        (statusTextPositionIsTop(SETTINGS.statusBarBookPageCounterPosition) == renderTopBand);
    const bool showBandProgressText = showBandPageCounter || showBandBookPercentage || showBandChapterPercentage ||
                                      showBandBookPageCounter;
    const bool showBandTitle = SETTINGS.statusBarShowChapterTitle && !statusBarLayout.titleLines.empty() &&
                               (statusBarItemIsTop(SETTINGS.statusBarTitlePosition) == renderTopBand);
    const bool showBandBookBar =
        SETTINGS.statusBarShowBookBar && (statusBarItemIsTop(SETTINGS.statusBarBookBarPosition) == renderTopBand);
    const bool showBandChapterBar =
        SETTINGS.statusBarShowChapterBar && (statusBarItemIsTop(SETTINGS.statusBarChapterBarPosition) == renderTopBand);
    const bool showStatusTextRow = showBandBattery || showBandProgressText;
    const int titleLineCount = showBandTitle ? static_cast<int>(statusBarLayout.titleLines.size()) : 0;
    const int textBlockHeight = computeStatusTextBlockHeight(renderer, statusFontId, showStatusTextRow, titleLineCount);
    const int activeBars = (showBandBookBar ? 1 : 0) + (showBandChapterBar ? 1 : 0);
    const int barsHeight =
        computeStatusBarsHeight(showBandBookBar, showBandChapterBar, progressBarHeight, textBlockHeight > 0);
    const int renderedBarsHeight = activeBars * progressBarHeight;

    if (debugBorders) {
      DrawUtils::drawDottedRect(renderer, orientedMarginLeft, bandTopY,
                                renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight, reservedHeight);
    }

    int currentTextY = bandTopY;
    if (textBlockHeight > 0) {
      currentTextY += kStatusTextTopPadding;
      if (renderTopBand && barsHeight > 0) {
        currentTextY += barsHeight;
      }
    }

    // When the title is in the top band, draw it first (outermost edge).
    // When in the bottom band, draw it last (outermost edge).
    const bool titleFirst = renderTopBand && showBandTitle;
    const int titleLineStep = textHeight + kStatusTextLineGap;
    const int titleBlockHeight =
        showBandTitle ? static_cast<int>(statusBarLayout.titleLines.size()) * titleLineStep : 0;

    int statusTextY = currentTextY;
    int titleY = currentTextY;
    if (titleFirst && showStatusTextRow) {
      statusTextY += titleBlockHeight;
    } else if (!titleFirst && showStatusTextRow) {
      titleY += textHeight + kStatusTextLineGap;
    }

    // Title rendering helper — used for both top-first and bottom-last positions.
    const auto drawTitle = [&]() {
      if (!showBandTitle) return;
      constexpr int titlePadding = 4;
      const int screenWidth = renderer.getScreenWidth();
      const int paddedWidth = screenWidth - titlePadding * 2;
      for (size_t i = 0; i < statusBarLayout.titleLines.size(); i++) {
        std::string lineText = statusBarLayout.titleLines[i];
        int titleWidth = (i < statusBarLayout.titleLineWidths.size())
                             ? statusBarLayout.titleLineWidths[i]
                             : renderer.getTextWidth(statusFontId, lineText.c_str());
        int titleX;
        if (titleWidth <= paddedWidth) {
          titleX = titlePadding + (paddedWidth - titleWidth) / 2;
        } else if (titleWidth <= screenWidth) {
          titleX = (screenWidth - titleWidth) / 2;
        } else {
          lineText = renderer.truncatedText(statusFontId, lineText.c_str(), screenWidth);
          titleWidth = renderer.getTextWidth(statusFontId, lineText.c_str());
          titleX = (screenWidth - titleWidth) / 2;
        }
        renderer.drawText(statusFontId, titleX, titleY + static_cast<int>(i) * titleLineStep,
                          lineText.c_str());
      }
    };

    if (titleFirst) {
      drawTitle();
    }

    const int batteryWidth = showBandBattery ? renderer.getTextWidth(statusFontId, "100%") : 0;

    if (showBandBattery || showBandProgressText) {
      struct TextEntry {
        const std::string* text;  // nullptr = battery item
        int width;
      };
      std::vector<TextEntry> leftItems;
      std::vector<TextEntry> centerItems;
      std::vector<TextEntry> rightItems;
      const auto addItem = [&](const bool enabled, const std::string* text, const int width, const uint8_t position) {
        if (!enabled) {
          return;
        }
        TextEntry entry{text, width};
        switch (statusTextPositionHorizontalSlot(position)) {
          case 0:
            leftItems.push_back(entry);
            break;
          case 2:
            rightItems.push_back(entry);
            break;
          case 1:
          default:
            centerItems.push_back(entry);
            break;
        }
      };
      addItem(showBandBattery, nullptr, batteryWidth,
              SETTINGS.statusBarBatteryPosition);
      addItem(showBandPageCounter, &statusBarLayout.pageCounterText, statusBarLayout.pageCounterTextWidth,
              SETTINGS.statusBarPageCounterPosition);
      addItem(showBandBookPercentage, &statusBarLayout.bookPercentageText, statusBarLayout.bookPercentageTextWidth,
              SETTINGS.statusBarBookPercentagePosition);
      addItem(showBandChapterPercentage, &statusBarLayout.chapterPercentageText,
              statusBarLayout.chapterPercentageTextWidth, SETTINGS.statusBarChapterPercentagePosition);
      addItem(showBandBookPageCounter, &statusBarLayout.bookPageCounterText,
              statusBarLayout.bookPageCounterTextWidth, SETTINGS.statusBarBookPageCounterPosition);

      const auto drawGroup = [&](const std::vector<TextEntry>& items, const int startX) {
        int x = startX;
        for (size_t i = 0; i < items.size(); i++) {
          if (i > 0) {
            x += kStatusItemGap;
          }
          if (items[i].text == nullptr) {
            GUI.drawBatteryLeft(renderer, Rect{x, statusTextY, metrics.batteryWidth, metrics.batteryHeight},
                                showBatteryPercentage);
          } else {
            renderer.drawText(statusFontId, x, statusTextY, items[i].text->c_str());
          }
          x += items[i].width;
        }
      };
      const auto groupWidth = [&](const std::vector<TextEntry>& items) {
        int width = 0;
        for (size_t i = 0; i < items.size(); i++) {
          if (i > 0) {
            width += kStatusItemGap;
          }
          width += items[i].width;
        }
        return width;
      };

      const int leftGroupWidth = groupWidth(leftItems);
      const int centerGroupWidth = groupWidth(centerItems);
      const int rightGroupWidth = groupWidth(rightItems);
      if (leftGroupWidth > 0) {
        drawGroup(leftItems, orientedMarginLeft);
      }
      if (centerGroupWidth > 0) {
        drawGroup(centerItems, orientedMarginLeft + std::max(0, (usableWidth - centerGroupWidth) / 2));
      }
      if (rightGroupWidth > 0) {
        drawGroup(rightItems, orientedMarginLeft + std::max(0, usableWidth - rightGroupWidth));
      }
    }

    if (!titleFirst) {
      drawTitle();
    }

    if (barsHeight <= 0) {
      return;
    }

    int barIndex = 0;
    int currentBarY = renderTopBand ? bandTopY : bandTopY + reservedHeight - renderedBarsHeight;
    const auto drawBandBar = [&](const size_t progressPercent) {
      const bool isFirstBar = barIndex == 0;
      const bool isLastBar = barIndex == activeBars - 1;
      int barY = currentBarY + barIndex * progressBarHeight;
      int barDrawHeight = progressBarHeight;
      if (renderTopBand && isFirstBar) {
        barY -= statusTopInset;
        barDrawHeight += statusTopInset;
      }
      if (!renderTopBand && isLastBar) {
        barDrawHeight += statusBottomInset;
      }
      drawStyledProgressBar(renderer, progressPercent, barY, barDrawHeight);
      barIndex++;
    };

    // Book bar is always at the outermost edge: first when top, last when bottom.
    if (renderTopBand) {
      if (showBandBookBar) {
        drawBandBar(static_cast<size_t>(statusBarLayout.bookProgress));
      }
      if (showBandChapterBar) {
        drawBandBar(static_cast<size_t>(statusBarLayout.chapterProgress));
      }
    } else {
      if (showBandChapterBar) {
        drawBandBar(static_cast<size_t>(statusBarLayout.chapterProgress));
      }
      if (showBandBookBar) {
        drawBandBar(static_cast<size_t>(statusBarLayout.bookProgress));
      }
    }
  };

  renderBand(statusTopInset, statusBarLayout.topReservedHeight, true);
  renderBand(screenHeight - statusBottomInset - statusBarLayout.bottomReservedHeight,
             statusBarLayout.bottomReservedHeight, false);
}

}  // namespace ReaderStatusBar
