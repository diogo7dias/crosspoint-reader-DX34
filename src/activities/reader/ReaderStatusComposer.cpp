#include "ReaderStatusComposer.h"

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"

namespace crosspoint {
namespace reader {

namespace {
// Position predicates, mirroring ReaderStatusBar's (trivial enum thresholds kept
// here so the composer needs no hardware-coupled ReaderStatusBar.cpp at link).
inline bool statusBarItemIsTop(const uint8_t position) { return position == CrossPointSettings::STATUS_AT_TOP; }
inline bool statusTextPositionIsTop(const uint8_t position) {
  return position <= CrossPointSettings::STATUS_TEXT_TOP_RIGHT;
}

// Matches the cap in EpubReaderActivity::getWrappedStatusBarReserveLineCount: a
// spine can carry hundreds of TOC entries and each cold-wrap costs ~140 ms, so
// sample at most this many — the first handful bound the max wrap count.
constexpr int kMaxTocTitlesMeasured = 8;
}  // namespace

ReaderStatusComposer::ReaderStatusComposer(const IStatusMeasurePort& measure, const char* logTag,
                                           StatusTitleHooks hooks)
    : measure_(measure), logTag_(logTag), hooks_(std::move(hooks)) {}

int ReaderStatusComposer::reserveTitleLineCountCached(const int fontId, const int wrapWidth,
                                                      const bool noTitleTruncation) {
  const int key = hooks_.reserveTitleKey ? hooks_.reserveTitleKey() : 0;
  if (reserveCache_.valid && reserveCache_.key == key && reserveCache_.width == wrapWidth &&
      reserveCache_.noTrunc == noTitleTruncation) {
    return reserveCache_.lineCount;
  }

  int maxLines = 1;
  // The hook caps its candidate set to kMaxTocTitlesMeasured (it owns the
  // domain knowledge of which entries to sample); empties don't contribute but
  // are not re-counted, matching getWrappedStatusBarReserveLineCount's loop.
  const std::vector<std::string> samples =
      hooks_.reserveTitleSamples ? hooks_.reserveTitleSamples(kMaxTocTitlesMeasured) : std::vector<std::string>{};
  for (const auto& title : samples) {
    if (title.empty()) continue;
    const int lineCount = static_cast<int>(ReaderLayoutSafety::wrapText(measure_, fontId, title, wrapWidth).size());
    if (lineCount > maxLines) maxLines = lineCount;
  }

  reserveCache_ = ReserveCache{true, key, wrapWidth, noTitleTruncation, maxLines};
  return maxLines;
}

const std::vector<std::string>& ReaderStatusComposer::displayTitleLinesCached(const int fontId, const int wrapWidth,
                                                                              const bool noTitleTruncation,
                                                                              const int maxLineCount) {
  const int key = hooks_.displayTitleKey ? hooks_.displayTitleKey() : 0;
  if (titleCache_.valid && titleCache_.key == key && titleCache_.width == wrapWidth &&
      titleCache_.noTrunc == noTitleTruncation && titleCache_.maxLines == maxLineCount) {
    return titleCache_.lines;
  }

  const std::string text = hooks_.displayTitleText ? hooks_.displayTitleText() : std::string();
  titleCache_.lines =
      ReaderLayoutSafety::buildTitleLines(measure_, fontId, text, wrapWidth, noTitleTruncation, maxLineCount);
  titleCache_.valid = true;
  titleCache_.key = key;
  titleCache_.width = wrapWidth;
  titleCache_.noTrunc = noTitleTruncation;
  titleCache_.maxLines = maxLineCount;
  return titleCache_.lines;
}

ReserveResult ReaderStatusComposer::reserve(const StatusBarSettings& s, const ReserveInput& in) {
  ReserveResult result;
  result.resolvedTitleLineCount = s.showChapterTitle ? 1 : 0;
  if (!s.enabled) {
    return result;
  }

  const bool showTopStatusTextRow = (s.showBattery && statusTextPositionIsTop(s.batteryPosition)) ||
                                    (s.showPageCounter && statusTextPositionIsTop(s.pageCounterPosition)) ||
                                    (s.showBookPercentage && statusTextPositionIsTop(s.bookPercentagePosition)) ||
                                    (s.showChapterPercentage && statusTextPositionIsTop(s.chapterPercentagePosition)) ||
                                    (s.showPagesLeft && statusTextPositionIsTop(s.pagesLeftPosition));
  const bool showBottomStatusTextRow =
      (s.showBattery && !statusTextPositionIsTop(s.batteryPosition)) ||
      (s.showPageCounter && !statusTextPositionIsTop(s.pageCounterPosition)) ||
      (s.showBookPercentage && !statusTextPositionIsTop(s.bookPercentagePosition)) ||
      (s.showChapterPercentage && !statusTextPositionIsTop(s.chapterPercentagePosition)) ||
      (s.showPagesLeft && !statusTextPositionIsTop(s.pagesLeftPosition));

  int titleLineCount = s.showChapterTitle ? 1 : 0;
  if (s.showChapterTitle && s.noTitleTruncation) {
    titleLineCount = reserveTitleLineCountCached(s.fontId, in.titleReserveWrapWidth, s.noTitleTruncation);
  }
  const int topTitleLineCount = (s.showChapterTitle && statusBarItemIsTop(s.titlePosition)) ? titleLineCount : 0;
  const int bottomTitleLineCount = (s.showChapterTitle && !statusBarItemIsTop(s.titlePosition)) ? titleLineCount : 0;

  const auto budget = ReaderLayoutSafety::resolveStatusBarBudget(
      measure_, s.fontId, logTag_, in.screenHeight, in.statusTopInset, in.statusBottomInset, in.marginTop,
      in.marginBottom, in.minContentHeight, s.progressBarHeight,
      ReaderLayoutSafety::StatusBarBandConfig{
          .showStatusTextRow = showTopStatusTextRow,
          .showBookProgressBar = s.showBookBar && statusBarItemIsTop(s.bookBarPosition),
          .showChapterProgressBar = s.showChapterBar && statusBarItemIsTop(s.chapterBarPosition),
          .desiredTitleLineCount = topTitleLineCount,
      },
      ReaderLayoutSafety::StatusBarBandConfig{
          .showStatusTextRow = showBottomStatusTextRow,
          .showBookProgressBar = s.showBookBar && !statusBarItemIsTop(s.bookBarPosition),
          .showChapterProgressBar = s.showChapterBar && !statusBarItemIsTop(s.chapterBarPosition),
          .desiredTitleLineCount = bottomTitleLineCount,
      });

  result.topReservedHeight = budget.top.reservedHeight;
  result.bottomReservedHeight = budget.bottom.reservedHeight;
  result.resolvedTitleLineCount =
      statusBarItemIsTop(s.titlePosition) ? budget.top.titleLineCount : budget.bottom.titleLineCount;
  return result;
}

ReaderStatusBar::StatusBarLayout ReaderStatusComposer::build(const StatusBarSettings& s, const int usableWidth,
                                                             const ReserveResult& reserved,
                                                             const StatusValues& values) {
  ReaderStatusBar::StatusBarLayout layout;
  layout.usableWidth = ReaderLayoutSafety::clampViewportDimension(usableWidth, ReaderLayoutSafety::kMinViewportWidth,
                                                                  logTag_, "status width");
  layout.topReservedHeight = reserved.topReservedHeight;
  layout.bottomReservedHeight = reserved.bottomReservedHeight;
  if (!s.enabled) {
    return layout;
  }

  layout.bookProgress = values.bookProgress;
  layout.chapterProgress = values.chapterProgress;

  if (s.showPageCounter && !values.pageCounterText.empty()) {
    layout.pageCounterText = values.pageCounterText;
    layout.pageCounterTextWidth = measure_.getTextWidth(s.fontId, layout.pageCounterText.c_str());
  }
  if (s.showBookPercentage) {
    char buf[16] = {0};
    snprintf(buf, sizeof(buf), "B:%.0f%%", layout.bookProgress);
    layout.bookPercentageText = buf;
    layout.bookPercentageTextWidth = measure_.getTextWidth(s.fontId, buf);
  }
  if (s.showChapterPercentage) {
    char buf[16] = {0};
    snprintf(buf, sizeof(buf), "C:%.0f%%", layout.chapterProgress);
    layout.chapterPercentageText = buf;
    layout.chapterPercentageTextWidth = measure_.getTextWidth(s.fontId, buf);
  }
  if (s.showPagesLeft && values.pageCount > 0) {
    // Pages remaining to the end of the current chapter/file. currentPage0 is
    // 0-based, so the last page yields 0 left.
    const int remaining = std::max(0, values.pageCount - (values.currentPage0 + 1));
    char buf[24];
    snprintf(buf, sizeof(buf), "%d %s", remaining, values.pagesLeftLabel);
    layout.pagesLeftText = buf;
    layout.pagesLeftTextWidth = measure_.getTextWidth(s.fontId, buf);
  }
  if (s.showChapterNumber && !values.chapterNumberText.empty()) {
    layout.chapterNumberText = values.chapterNumberText;
    layout.chapterNumberTextWidth = measure_.getTextWidth(s.fontId, layout.chapterNumberText.c_str());
  }
  if (s.showQuoteCount && !values.quoteCountText.empty()) {
    layout.quoteCountText = values.quoteCountText;
    layout.quoteCountTextWidth = measure_.getTextWidth(s.fontId, layout.quoteCountText.c_str());
  }
  if (s.showFreeHeap) {
    char buf[24];
    snprintf(buf, sizeof(buf), "RAM %uK", static_cast<unsigned>(values.freeHeapBytes / 1024));
    layout.freeHeapText = buf;
    layout.freeHeapTextWidth = measure_.getTextWidth(s.fontId, buf);
  }
  if (s.showChapterTitle) {
    constexpr int titlePadding = 4;
    const int titleWrapWidth = measure_.getScreenWidth() - titlePadding * 2;
    layout.titleLines =
        displayTitleLinesCached(s.fontId, titleWrapWidth, s.noTitleTruncation, reserved.resolvedTitleLineCount);
    layout.titleLineWidths.reserve(layout.titleLines.size());
    for (const auto& line : layout.titleLines) {
      layout.titleLineWidths.push_back(measure_.getTextWidth(s.fontId, line.c_str()));
    }
  }
  return layout;
}

void ReaderStatusComposer::invalidateTitleCaches() {
  reserveCache_.valid = false;
  titleCache_.valid = false;
}

}  // namespace reader
}  // namespace crosspoint
