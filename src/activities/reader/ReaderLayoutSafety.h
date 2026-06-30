#pragma once

#include <string>
#include <vector>

#include "IStatusMeasurePort.h"

// Forward declaration only: the GfxRenderer overloads below take it by reference,
// so this header stays free of GfxRenderer.h (and its HalDisplay weld) and can be
// included by host-test translation units. The overload definitions live in the
// device-only ReaderLayoutSafetyGfx.cpp; the port-based bodies in ReaderLayoutSafety.cpp.
class GfxRenderer;

namespace ReaderLayoutSafety {

using crosspoint::reader::IStatusMeasurePort;

constexpr int kMinViewportWidth = 64;
constexpr int kMinViewportHeight = 48;

// Oriented page margins (top/right/bottom/left, in device pixels). Bundles the four sides so
// render() passes one value instead of four positional ints — the order-of-args footgun was a
// recurring source of bugs during the V2 render stack refactor. Callers that pre-date this struct
// can still copy the fields into individual locals at the interior boundary.
struct ReaderMargins {
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
};

// Computes the base (pre-status-bar) oriented page margins for the reader: takes the hardware
// viewable TRBL, normalizes top/bottom and left/right pairs to the larger of each (so the page
// stays visually centered when the driver reports asymmetric insets), adds the user-configured
// margins, and optionally widens horizontal margins to target ~62 characters per line.
//
// WHY separate from status-bar reserve: the caller still has to run resolveStatusBarBudget after
// this — and that may further push the top/bottom in based on reserved status-bar height. Folding
// both into one call would couple this helper to the much larger StatusBarBandConfig surface.
// readerFontId is only consulted when dynamicMargins is non-zero (for the glyph-width sample);
// callers with dynamicMargins==0 can pass any valid font id.
ReaderMargins resolveBaseReaderMargins(const GfxRenderer& renderer, int userMarginTop, int userMarginBottom,
                                       int userMarginHorizontal, int dynamicMargins, int readerFontId);

struct StatusBarBandConfig {
  bool showStatusTextRow = false;
  bool showBookProgressBar = false;
  bool showChapterProgressBar = false;
  int desiredTitleLineCount = 0;
};

struct StatusBarBandBudget {
  int reservedHeight = 0;
  int titleLineCount = 0;
};

struct StatusBarBudgetResult {
  StatusBarBandBudget top;
  StatusBarBandBudget bottom;
};

// Measurement-dependent helpers come in two overloads each: the primary takes an
// IStatusMeasurePort (host-testable with a fake measurer), the convenience overload
// takes a GfxRenderer and forwards through a ProdStatusMeasurePort. Existing callers
// pass a GfxRenderer unchanged; the status-bar composer + host tests pass a port.
std::vector<std::string> wrapText(const IStatusMeasurePort& measure, int fontId, const std::string& text, int maxWidth);
std::vector<std::string> wrapText(const GfxRenderer& renderer, int fontId, const std::string& text, int maxWidth);

std::vector<std::string> buildTitleLines(const IStatusMeasurePort& measure, int fontId, const std::string& text,
                                         int maxWidth, bool noTitleTruncation, int maxLineCount);
std::vector<std::string> buildTitleLines(const GfxRenderer& renderer, int fontId, const std::string& text, int maxWidth,
                                         bool noTitleTruncation, int maxLineCount);

int computeStatusTextBlockHeight(const IStatusMeasurePort& measure, int fontId, bool showStatusTextRow,
                                 int titleLineCount);
int computeStatusTextBlockHeight(const GfxRenderer& renderer, int fontId, bool showStatusTextRow, int titleLineCount);

int computeStatusBarsHeight(bool showBookProgressBar, bool showChapterProgressBar, int statusBarProgressHeight,
                            bool includeTopMargin);

int computeReservedHeight(const IStatusMeasurePort& measure, int fontId, bool showStatusTextRow,
                          bool showBookProgressBar, bool showChapterProgressBar, int titleLineCount,
                          int statusBarProgressHeight);
int computeReservedHeight(const GfxRenderer& renderer, int fontId, bool showStatusTextRow, bool showBookProgressBar,
                          bool showChapterProgressBar, int titleLineCount, int statusBarProgressHeight);

StatusBarBudgetResult resolveStatusBarBudget(const IStatusMeasurePort& measure, int fontId, const char* logTag,
                                             int screenHeight, int statusTopInset, int statusBottomInset, int marginTop,
                                             int marginBottom, int minContentHeight, int statusBarProgressHeight,
                                             const StatusBarBandConfig& topConfig,
                                             const StatusBarBandConfig& bottomConfig);
StatusBarBudgetResult resolveStatusBarBudget(const GfxRenderer& renderer, int fontId, const char* logTag,
                                             int screenHeight, int statusTopInset, int statusBottomInset, int marginTop,
                                             int marginBottom, int minContentHeight, int statusBarProgressHeight,
                                             const StatusBarBandConfig& topConfig,
                                             const StatusBarBandConfig& bottomConfig);
int clampViewportDimension(int value, int minValue, const char* logTag, const char* dimensionName);

}  // namespace ReaderLayoutSafety
