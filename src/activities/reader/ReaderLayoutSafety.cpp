#include "ReaderLayoutSafety.h"

#include <algorithm>
#include <utility>

// Logging is a hardware-coupled lib excluded from the host test build; shim it
// to no-ops there (same pattern as SettingsCodec.cpp). This TU holds only the
// port-based logic so it compiles host-side; the GfxRenderer overloads live in
// ReaderLayoutSafetyGfx.cpp (device-only).
#if defined(UNIT_TEST_HOST)
#define LOG_DBG(...) ((void)0)
#else
#include <Logging.h>
#endif

using crosspoint::reader::IStatusMeasurePort;

namespace ReaderLayoutSafety {
namespace {
constexpr int kProgressBarMarginTop = 1;
constexpr int kStatusTextTopPadding = 4;
constexpr int kStatusTextBottomPadding = 4;
constexpr int kStatusTextLineGap = 1;
constexpr int kStatusTextToBarsGap = 0;

StatusBarBandBudget makeBandBudget(const IStatusMeasurePort& measure, const int fontId,
                                   const StatusBarBandConfig& config, const int titleLineCount,
                                   const int statusBarProgressHeight) {
  StatusBarBandBudget budget;
  budget.titleLineCount = std::max(0, titleLineCount);
  budget.reservedHeight =
      computeReservedHeight(measure, fontId, config.showStatusTextRow, config.showBookProgressBar,
                            config.showChapterProgressBar, budget.titleLineCount, statusBarProgressHeight);
  return budget;
}
}  // namespace

std::vector<std::string> wrapText(const IStatusMeasurePort& measure, const int fontId, const std::string& text,
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

      if (measure.getTextWidth(fontId, candidate.c_str()) <= maxWidth) {
        line = candidate;
        lineEndPos = wordEnd;
        while (lineEndPos < text.size() && text[lineEndPos] == ' ') {
          lineEndPos++;
        }
        continue;
      }

      if (line.empty()) {
        size_t fit = 1;
        while (fit < word.size() && measure.getTextWidth(fontId, word.substr(0, fit + 1).c_str()) <= maxWidth) {
          fit++;
        }
        line = word.substr(0, fit);
        lineEndPos += fit;
      }
      break;
    }

    if (line.empty()) {
      lines.push_back(measure.truncatedText(fontId, text.substr(i).c_str(), maxWidth));
      break;
    }

    lines.push_back(std::move(line));
    i = lineEndPos;
  }

  if (lines.empty()) {
    lines.push_back(measure.truncatedText(fontId, text.c_str(), maxWidth));
  }
  return lines;
}

std::vector<std::string> buildTitleLines(const IStatusMeasurePort& measure, const int fontId, const std::string& text,
                                         const int maxWidth, const bool noTitleTruncation, const int maxLineCount) {
  if (text.empty() || maxLineCount <= 0 || maxWidth <= 0) {
    return {};
  }

  if (!noTitleTruncation || maxLineCount == 1) {
    return {measure.truncatedText(fontId, text.c_str(), maxWidth)};
  }

  std::vector<std::string> wrapped = wrapText(measure, fontId, text, maxWidth);
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
  wrapped.back() = measure.truncatedText(fontId, finalLine.c_str(), maxWidth);
  return wrapped;
}

int computeStatusTextBlockHeight(const IStatusMeasurePort& measure, const int fontId, const bool showStatusTextRow,
                                 const int titleLineCount) {
  const int statusTextHeight = measure.getTextHeight(fontId);
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

int computeReservedHeight(const IStatusMeasurePort& measure, const int fontId, const bool showStatusTextRow,
                          const bool showBookProgressBar, const bool showChapterProgressBar, const int titleLineCount,
                          const int statusBarProgressHeight) {
  const int textBlockHeight = computeStatusTextBlockHeight(measure, fontId, showStatusTextRow, titleLineCount);
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

StatusBarBudgetResult resolveStatusBarBudget(const IStatusMeasurePort& measure, const int fontId, const char* logTag,
                                             const int screenHeight, const int statusTopInset,
                                             const int statusBottomInset, const int marginTop, const int marginBottom,
                                             const int minContentHeight, const int statusBarProgressHeight,
                                             const StatusBarBandConfig& topConfig,
                                             const StatusBarBandConfig& bottomConfig) {
  StatusBarBudgetResult result;
  result.top = makeBandBudget(measure, fontId, topConfig, topConfig.desiredTitleLineCount, statusBarProgressHeight);
  result.bottom =
      makeBandBudget(measure, fontId, bottomConfig, bottomConfig.desiredTitleLineCount, statusBarProgressHeight);

  const int availableStatusHeight =
      std::max(0, screenHeight - statusTopInset - statusBottomInset - marginTop - marginBottom - minContentHeight);
  int totalReserved = result.top.reservedHeight + result.bottom.reservedHeight;

  while (totalReserved > availableStatusHeight && (result.top.titleLineCount > 0 || result.bottom.titleLineCount > 0)) {
    const bool reduceTop = result.top.titleLineCount > result.bottom.titleLineCount ||
                           (result.top.titleLineCount == result.bottom.titleLineCount &&
                            result.top.reservedHeight >= result.bottom.reservedHeight);
    if (reduceTop && result.top.titleLineCount > 0) {
      result.top = makeBandBudget(measure, fontId, topConfig, result.top.titleLineCount - 1, statusBarProgressHeight);
    } else if (result.bottom.titleLineCount > 0) {
      result.bottom =
          makeBandBudget(measure, fontId, bottomConfig, result.bottom.titleLineCount - 1, statusBarProgressHeight);
    } else if (result.top.titleLineCount > 0) {
      result.top = makeBandBudget(measure, fontId, topConfig, result.top.titleLineCount - 1, statusBarProgressHeight);
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

int clampViewportDimension(const int value, const int minValue, const char* logTag, const char* dimensionName) {
  if (value >= minValue) {
    return value;
  }

  LOG_DBG(logTag, "Clamped %s: %d -> %d", dimensionName, value, minValue);
  return minValue;
}

}  // namespace ReaderLayoutSafety
