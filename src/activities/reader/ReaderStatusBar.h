#pragma once

#include <GfxRenderer.h>

#include <string>
#include <vector>

#include "StatusBarLayout.h"

namespace ReaderStatusBar {

// Constants shared by both readers' status bar rendering.
constexpr int kStatusTextTopPadding = 4;
constexpr int kStatusTextLineGap = 1;
constexpr int kStatusTextToBarsGap = 0;
constexpr int kStatusItemGap = 12;

// StatusBarLayout now lives in StatusBarLayout.h (host-safe, no GfxRenderer).

// --- Shared helpers (previously duplicated in both reader .cpp files) ---

void drawStyledProgressBar(const GfxRenderer& renderer, size_t progressPercent, int y, int height);
void normalizeReaderMargins(int* top, int* right, int* bottom, int* left);
int getStatusBottomInset(const GfxRenderer& renderer);
int getStatusTopInset(const GfxRenderer& renderer);
bool statusBarItemIsTop(uint8_t position);
bool statusTextPositionIsTop(uint8_t position);
int statusTextPositionHorizontalSlot(uint8_t position);
int computeStatusTextBlockHeight(const GfxRenderer& renderer, int fontId, bool showStatusTextRow, int titleLineCount);
int computeStatusBarsHeight(bool showBookProgressBar, bool showChapterProgressBar, int statusBarProgressHeight,
                            bool includeTopMargin);
int computeStatusBarReservedHeight(const GfxRenderer& renderer, int fontId, bool showStatusTextRow,
                                   bool showBookProgressBar, bool showChapterProgressBar, int titleLineCount);
std::vector<std::string> wrapStatusText(const GfxRenderer& renderer, int fontId, const std::string& text, int maxWidth);

// Main rendering function — called by both EPUB and TXT readers.
void renderStatusBar(GfxRenderer& renderer, const StatusBarLayout& layout, int orientedMarginRight,
                     int orientedMarginBottom, int orientedMarginLeft, bool debugBorders);

}  // namespace ReaderStatusBar
