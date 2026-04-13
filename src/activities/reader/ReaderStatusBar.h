#pragma once

#include <GfxRenderer.h>

#include <string>
#include <vector>

namespace ReaderStatusBar {

// Constants shared by both readers' status bar rendering.
constexpr int kStatusTextTopPadding = 4;
constexpr int kStatusTextLineGap = 1;
constexpr int kStatusTextToBarsGap = 0;
constexpr int kStatusItemGap = 12;

// Unified status bar layout populated by each reader's buildStatusBarLayout().
// TXT readers set bookProgress and chapterProgress to the same value.
// titleLineWidths is optional — when empty, widths are computed at render time.
struct StatusBarLayout {
  int topReservedHeight = 0;
  int bottomReservedHeight = 0;
  int usableWidth = 0;
  std::string pageCounterText;
  int pageCounterTextWidth = 0;
  std::string bookPercentageText;
  int bookPercentageTextWidth = 0;
  std::string chapterPercentageText;
  int chapterPercentageTextWidth = 0;
  std::string bookPageCounterText;
  int bookPageCounterTextWidth = 0;
  std::vector<std::string> titleLines;
  std::vector<int> titleLineWidths;
  float bookProgress = 0.0f;
  float chapterProgress = 0.0f;
};

// --- Shared helpers (previously duplicated in both reader .cpp files) ---

void drawStyledProgressBar(const GfxRenderer& renderer, size_t progressPercent, int y, int height);
void normalizeReaderMargins(int* top, int* right, int* bottom, int* left);
int getStatusBottomInset(const GfxRenderer& renderer);
int getStatusTopInset(const GfxRenderer& renderer);
bool statusBarItemIsTop(uint8_t position);
bool statusTextPositionIsTop(uint8_t position);
int statusTextPositionHorizontalSlot(uint8_t position);
int computeStatusTextBlockHeight(const GfxRenderer& renderer, int fontId, bool showStatusTextRow, int titleLineCount);
int computeStatusBarsHeight(bool showBookProgressBar, bool showChapterProgressBar,
                            int statusBarProgressHeight, bool includeTopMargin);
int computeStatusBarReservedHeight(const GfxRenderer& renderer, int fontId, bool showStatusTextRow,
                                   bool showBookProgressBar, bool showChapterProgressBar, int titleLineCount);
std::vector<std::string> wrapStatusText(const GfxRenderer& renderer, int fontId,
                                        const std::string& text, int maxWidth);

// Main rendering function — called by both EPUB and TXT readers.
void renderStatusBar(GfxRenderer& renderer, const StatusBarLayout& layout,
                     int orientedMarginRight, int orientedMarginBottom,
                     int orientedMarginLeft, bool debugBorders);

}  // namespace ReaderStatusBar
