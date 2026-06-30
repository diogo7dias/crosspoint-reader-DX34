#pragma once

#include <string>
#include <vector>

/**
 * @file StatusBarLayout.h
 * @brief The reader status-bar layout POD, split out so it carries no GfxRenderer
 *        dependency and can be produced/asserted in host tests.
 *
 * Populated by ReaderStatusComposer::build() and consumed by
 * ReaderStatusBar::renderStatusBar(). Empty string => the corresponding item is
 * not drawn. TXT readers set bookProgress and chapterProgress to the same value.
 * titleLineWidths is optional — when empty, widths are computed at render time.
 */
namespace ReaderStatusBar {

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
  std::string pagesLeftText;
  int pagesLeftTextWidth = 0;
  std::string chapterNumberText;
  int chapterNumberTextWidth = 0;
  std::string quoteCountText;
  int quoteCountTextWidth = 0;
  std::string freeHeapText;
  int freeHeapTextWidth = 0;
  std::vector<std::string> titleLines;
  std::vector<int> titleLineWidths;
  float bookProgress = 0.0f;
  float chapterProgress = 0.0f;
};

}  // namespace ReaderStatusBar
