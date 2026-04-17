#pragma once

#include <GfxRenderer.h>

#include <string>
#include <vector>

namespace ReaderLayoutSafety {

constexpr int kMinViewportWidth = 64;
constexpr int kMinViewportHeight = 48;

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

std::vector<std::string> wrapText(const GfxRenderer& renderer, int fontId, const std::string& text, int maxWidth);
std::vector<std::string> buildTitleLines(const GfxRenderer& renderer, int fontId, const std::string& text, int maxWidth,
                                         bool noTitleTruncation, int maxLineCount);
int computeStatusTextBlockHeight(const GfxRenderer& renderer, int fontId, bool showStatusTextRow, int titleLineCount);
int computeStatusBarsHeight(bool showBookProgressBar, bool showChapterProgressBar, int statusBarProgressHeight,
                            bool includeTopMargin);
int computeReservedHeight(const GfxRenderer& renderer, int fontId, bool showStatusTextRow, bool showBookProgressBar,
                          bool showChapterProgressBar, int titleLineCount, int statusBarProgressHeight);
StatusBarBudgetResult resolveStatusBarBudget(const GfxRenderer& renderer, int fontId, const char* logTag,
                                             int screenHeight, int statusTopInset, int statusBottomInset, int marginTop,
                                             int marginBottom, int minContentHeight, int statusBarProgressHeight,
                                             const StatusBarBandConfig& topConfig,
                                             const StatusBarBandConfig& bottomConfig);
int clampViewportDimension(int value, int minValue, const char* logTag, const char* dimensionName);

}  // namespace ReaderLayoutSafety
