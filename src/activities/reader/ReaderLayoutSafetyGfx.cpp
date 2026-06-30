#include <GfxRenderer.h>

#include <algorithm>

#include "ReaderLayoutSafety.h"
#include "StatusBarPorts.h"

/**
 * @file ReaderLayoutSafetyGfx.cpp
 * @brief Device-only GfxRenderer adapters for ReaderLayoutSafety.
 *
 * The measurement-dependent helpers carry their logic in ReaderLayoutSafety.cpp
 * against an IStatusMeasurePort (host-testable). These convenience overloads let
 * existing callers keep passing a GfxRenderer unchanged — they wrap it in a
 * ProdStatusMeasurePort and forward. Kept in a separate translation unit so the
 * port-based bodies stay free of GfxRenderer and compile in the host test build.
 *
 * resolveBaseReaderMargins lives here too: it reads the live oriented viewable
 * insets and the reader font's glyph width straight off GfxRenderer, so it is
 * inherently device-side (no status-bar composer consumes it).
 */
namespace ReaderLayoutSafety {

using crosspoint::reader::ProdStatusMeasurePort;

std::vector<std::string> wrapText(const GfxRenderer& renderer, const int fontId, const std::string& text,
                                  const int maxWidth) {
  const ProdStatusMeasurePort measure(renderer);
  return wrapText(measure, fontId, text, maxWidth);
}

std::vector<std::string> buildTitleLines(const GfxRenderer& renderer, const int fontId, const std::string& text,
                                         const int maxWidth, const bool noTitleTruncation, const int maxLineCount) {
  const ProdStatusMeasurePort measure(renderer);
  return buildTitleLines(measure, fontId, text, maxWidth, noTitleTruncation, maxLineCount);
}

int computeStatusTextBlockHeight(const GfxRenderer& renderer, const int fontId, const bool showStatusTextRow,
                                 const int titleLineCount) {
  const ProdStatusMeasurePort measure(renderer);
  return computeStatusTextBlockHeight(measure, fontId, showStatusTextRow, titleLineCount);
}

int computeReservedHeight(const GfxRenderer& renderer, const int fontId, const bool showStatusTextRow,
                          const bool showBookProgressBar, const bool showChapterProgressBar, const int titleLineCount,
                          const int statusBarProgressHeight) {
  const ProdStatusMeasurePort measure(renderer);
  return computeReservedHeight(measure, fontId, showStatusTextRow, showBookProgressBar, showChapterProgressBar,
                               titleLineCount, statusBarProgressHeight);
}

StatusBarBudgetResult resolveStatusBarBudget(const GfxRenderer& renderer, const int fontId, const char* logTag,
                                             const int screenHeight, const int statusTopInset,
                                             const int statusBottomInset, const int marginTop, const int marginBottom,
                                             const int minContentHeight, const int statusBarProgressHeight,
                                             const StatusBarBandConfig& topConfig,
                                             const StatusBarBandConfig& bottomConfig) {
  const ProdStatusMeasurePort measure(renderer);
  return resolveStatusBarBudget(measure, fontId, logTag, screenHeight, statusTopInset, statusBottomInset, marginTop,
                                marginBottom, minContentHeight, statusBarProgressHeight, topConfig, bottomConfig);
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

}  // namespace ReaderLayoutSafety
