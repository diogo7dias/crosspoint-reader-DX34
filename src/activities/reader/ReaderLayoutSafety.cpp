#include "ReaderLayoutSafety.h"

#include <Logging.h>

#include <algorithm>
#include <utility>

#include "fontIds.h"

namespace ReaderLayoutSafety {
namespace {
constexpr int kProgressBarMarginTop = 1;
constexpr int kStatusTextTopPadding = 4;
constexpr int kStatusTextBottomPadding = 4;
constexpr int kStatusTextLineGap = 1;
constexpr int kStatusTextToBarsGap = 0;

StatusBarBandBudget makeBandBudget(const GfxRenderer& renderer, const int fontId, const StatusBarBandConfig& config,
                                   const int titleLineCount, const int statusBarProgressHeight) {
  StatusBarBandBudget budget;
  budget.titleLineCount = std::max(0, titleLineCount);
  budget.reservedHeight =
      computeReservedHeight(renderer, fontId, config.showStatusTextRow, config.showBookProgressBar,
                            config.showChapterProgressBar, budget.titleLineCount, statusBarProgressHeight);
  return budget;
}
}  // namespace

std::vector<std::string> wrapText(const GfxRenderer& renderer, const int fontId, const std::string& text,
                                  const int maxWidth) {
  if (text.empty()) {
    return {};
  }
  if (maxWidth <= 0) {
    return {text};
  }

  std::vector<std::string> lines;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && text[i] == ' ') {
      i++;
    }
    if (i >= text.size()) {
      break;
    }

    std::string line;
    size_t lineEndPos = i;
    while (lineEndPos < text.size()) {
      size_t wordEnd = lineEndPos;
      while (wordEnd < text.size() && text[wordEnd] != ' ') {
        wordEnd++;
      }
      const std::string word = text.substr(lineEndPos, wordEnd - lineEndPos);
      const std::string candidate = line.empty() ? word : (line + " " + word);

      if (renderer.getTextWidth(fontId, candidate.c_str()) <= maxWidth) {
        line = candidate;
        lineEndPos = wordEnd;
        while (lineEndPos < text.size() && text[lineEndPos] == ' ') {
          lineEndPos++;
        }
        continue;
      }

      if (line.empty()) {
        size_t fit = 1;
        while (fit < word.size() && renderer.getTextWidth(fontId, word.substr(0, fit + 1).c_str()) <= maxWidth) {
          fit++;
        }
        line = word.substr(0, fit);
        lineEndPos += fit;
      }
      break;
    }

    if (line.empty()) {
      lines.push_back(renderer.truncatedText(fontId, text.substr(i).c_str(), maxWidth));
      break;
    }

    lines.push_back(std::move(line));
    i = lineEndPos;
  }

  if (lines.empty()) {
    lines.push_back(renderer.truncatedText(fontId, text.c_str(), maxWidth));
  }
  return lines;
}

std::vector<std::string> buildTitleLines(const GfxRenderer& renderer, const int fontId, const std::string& text,
                                         const int maxWidth, const bool noTitleTruncation, const int maxLineCount) {
  if (text.empty() || maxLineCount <= 0 || maxWidth <= 0) {
    return {};
  }

  if (!noTitleTruncation || maxLineCount == 1) {
    return {renderer.truncatedText(fontId, text.c_str(), maxWidth)};
  }

  std::vector<std::string> wrapped = wrapText(renderer, fontId, text, maxWidth);
  if (static_cast<int>(wrapped.size()) <= maxLineCount) {
    return wrapped;
  }

  const std::vector<std::string> allWrapped = wrapped;
  wrapped.resize(static_cast<size_t>(maxLineCount));
  std::string finalLine = wrapped.back();
  for (size_t i = static_cast<size_t>(maxLineCount); i < allWrapped.size(); i++) {
    if (!finalLine.empty()) {
      finalLine += " ";
    }
    finalLine += allWrapped[i];
  }
  wrapped.back() = renderer.truncatedText(fontId, finalLine.c_str(), maxWidth);
  return wrapped;
}

int computeStatusTextBlockHeight(const GfxRenderer& renderer, const int fontId, const bool showStatusTextRow,
                                 const int titleLineCount) {
  const int statusTextHeight = renderer.getTextHeight(fontId);
  int textBlockHeight = 0;
  if (showStatusTextRow) {
    textBlockHeight += statusTextHeight;
  }
  if (titleLineCount > 0) {
    if (textBlockHeight > 0) {
      textBlockHeight += kStatusTextLineGap;
    }
    textBlockHeight += titleLineCount * statusTextHeight + (titleLineCount - 1) * kStatusTextLineGap;
  }
  return textBlockHeight;
}

int computeStatusBarsHeight(const bool showBookProgressBar, const bool showChapterProgressBar,
                            const int statusBarProgressHeight, const bool includeTopMargin) {
  const int activeBars = (showBookProgressBar ? 1 : 0) + (showChapterProgressBar ? 1 : 0);
  if (activeBars == 0) {
    return 0;
  }
  return activeBars * statusBarProgressHeight + (activeBars - 1) * 0 + (includeTopMargin ? kProgressBarMarginTop : 0);
}

int computeReservedHeight(const GfxRenderer& renderer, const int fontId, const bool showStatusTextRow,
                          const bool showBookProgressBar, const bool showChapterProgressBar, const int titleLineCount,
                          const int statusBarProgressHeight) {
  const int textBlockHeight = computeStatusTextBlockHeight(renderer, fontId, showStatusTextRow, titleLineCount);
  const int barsHeight = computeStatusBarsHeight(showBookProgressBar, showChapterProgressBar, statusBarProgressHeight,
                                                 textBlockHeight > 0);
  int reservedHeight = 0;
  if (textBlockHeight > 0) {
    reservedHeight += kStatusTextTopPadding + textBlockHeight;
  }
  if (barsHeight > 0) {
    if (textBlockHeight > 0) {
      reservedHeight += kStatusTextToBarsGap;
    }
    reservedHeight += barsHeight;
  }
  if (reservedHeight > 0) {
    reservedHeight += kStatusTextBottomPadding;
  }
  return reservedHeight;
}

StatusBarBudgetResult resolveStatusBarBudget(const GfxRenderer& renderer, const int fontId, const char* logTag,
                                             const int screenHeight, const int statusTopInset,
                                             const int statusBottomInset, const int marginTop, const int marginBottom,
                                             const int minContentHeight, const int statusBarProgressHeight,
                                             const StatusBarBandConfig& topConfig,
                                             const StatusBarBandConfig& bottomConfig) {
  StatusBarBudgetResult result;
  result.top = makeBandBudget(renderer, fontId, topConfig, topConfig.desiredTitleLineCount, statusBarProgressHeight);
  result.bottom =
      makeBandBudget(renderer, fontId, bottomConfig, bottomConfig.desiredTitleLineCount, statusBarProgressHeight);

  const int availableStatusHeight =
      std::max(0, screenHeight - statusTopInset - statusBottomInset - marginTop - marginBottom - minContentHeight);
  int totalReserved = result.top.reservedHeight + result.bottom.reservedHeight;

  while (totalReserved > availableStatusHeight && (result.top.titleLineCount > 0 || result.bottom.titleLineCount > 0)) {
    const bool reduceTop = result.top.titleLineCount > result.bottom.titleLineCount ||
                           (result.top.titleLineCount == result.bottom.titleLineCount &&
                            result.top.reservedHeight >= result.bottom.reservedHeight);
    if (reduceTop && result.top.titleLineCount > 0) {
      result.top = makeBandBudget(renderer, fontId, topConfig, result.top.titleLineCount - 1, statusBarProgressHeight);
    } else if (result.bottom.titleLineCount > 0) {
      result.bottom =
          makeBandBudget(renderer, fontId, bottomConfig, result.bottom.titleLineCount - 1, statusBarProgressHeight);
    } else if (result.top.titleLineCount > 0) {
      result.top = makeBandBudget(renderer, fontId, topConfig, result.top.titleLineCount - 1, statusBarProgressHeight);
    }
    totalReserved = result.top.reservedHeight + result.bottom.reservedHeight;
  }

  if (result.top.titleLineCount < topConfig.desiredTitleLineCount) {
    LOG_DBG(logTag, "Status title lines capped (top): %d -> %d", topConfig.desiredTitleLineCount,
            result.top.titleLineCount);
  }
  if (result.bottom.titleLineCount < bottomConfig.desiredTitleLineCount) {
    LOG_DBG(logTag, "Status title lines capped (bottom): %d -> %d", bottomConfig.desiredTitleLineCount,
            result.bottom.titleLineCount);
  }

  totalReserved = result.top.reservedHeight + result.bottom.reservedHeight;
  if (totalReserved > availableStatusHeight) {
    int overflow = totalReserved - availableStatusHeight;
    const int reduceBottom = std::min(result.bottom.reservedHeight, overflow);
    result.bottom.reservedHeight -= reduceBottom;
    overflow -= reduceBottom;
    if (overflow > 0) {
      const int reduceTop = std::min(result.top.reservedHeight, overflow);
      result.top.reservedHeight -= reduceTop;
    }
    LOG_DBG(logTag, "Status bar reserve clamped to preserve viewport: top=%d bottom=%d", result.top.reservedHeight,
            result.bottom.reservedHeight);
  }

  return result;
}

ReaderMargins resolveBaseReaderMargins(const GfxRenderer& renderer, const int userMarginTop, const int userMarginBottom,
                                       const int userMarginHorizontal, const int dynamicMargins,
                                       const int readerFontId) {
  ReaderMargins m;
  renderer.getOrientedViewableTRBL(&m.top, &m.right, &m.bottom, &m.left);

  // Normalize pairs to the larger of each so the page stays visually centered even when the
  // driver reports asymmetric hardware insets. Kept inline (not a call to
  // ReaderStatusBar::normalizeReaderMargins) to keep this module free of reverse deps.
  const int vertical = std::max(m.top, m.bottom);
  const int horizontal = std::max(m.left, m.right);
  m.top = vertical;
  m.bottom = vertical;
  m.left = horizontal;
  m.right = horizontal;

  m.top += userMarginTop;
  m.bottom += userMarginBottom;
  if (dynamicMargins) {
    // Dynamic horizontal margins: widen toward a target ~62 characters per line using the
    // current reader font's average glyph width as the yardstick. Pathological fonts with zero
    // width fall back to 8 px/char to avoid divide-by-zero; the floor/ceiling (10/20 .. 55)
    // prevents the auto-widen from eating the viewport on narrow orientations.
    const int sampleWidth = renderer.getTextWidth(readerFontId, "abcdefghijklmnopqrstuvwxyz");
    const int avgCharWidth = (sampleWidth > 0) ? sampleWidth / 26 : 8;
    constexpr int targetCPL = 62;
    const int targetTextWidth = targetCPL * avgCharWidth;
    const int availableWidth = renderer.getScreenWidth() - m.left - m.right;
    const int minDynamicMargin = (dynamicMargins >= 2) ? 20 : 10;
    const int dynamicMargin = std::max(minDynamicMargin, std::min(55, (availableWidth - targetTextWidth) / 2));
    m.left += dynamicMargin;
    m.right += dynamicMargin;
  } else {
    m.left += userMarginHorizontal;
    m.right += userMarginHorizontal;
  }
  return m;
}

int clampViewportDimension(const int value, const int minValue, const char* logTag, const char* dimensionName) {
  if (value >= minValue) {
    return value;
  }

  LOG_DBG(logTag, "Clamped %s: %d -> %d", dimensionName, value, minValue);
  return minValue;
}

}  // namespace ReaderLayoutSafety
