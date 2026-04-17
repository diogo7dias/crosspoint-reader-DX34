#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "fontIds.h"

class GfxRenderer;
struct RecentBook;

struct Rect {
  int x;
  int y;
  int width;
  int height;

  explicit Rect(int x = 0, int y = 0, int width = 0, int height = 0) : x(x), y(y), width(width), height(height) {}
};

struct TabInfo {
  const char* label;
  bool selected;
};

struct BookListVisibility {
  int firstVisible;  // Index of first fully-rendered book
  int lastVisible;   // Index of last fully-rendered book (inclusive)
  int totalCount;
};

struct ThemeMetrics {
  int batteryWidth;
  int batteryHeight;

  int topPadding;
  int batteryBarHeight;
  int headerHeight;
  int verticalSpacing;

  int contentSidePadding;
  int listRowHeight;
  int listWithSubtitleRowHeight;
  int menuRowHeight;
  int menuSpacing;

  int tabSpacing;
  int tabBarHeight;

  int scrollBarWidth;
  int scrollBarRightOffset;

  int homeTopPadding;
  int homeCoverHeight;
  int homeCoverTileHeight;
  int homeRecentBooksCount;

  int buttonHintsHeight;
  int sideButtonHintsWidth;

  int versionTextRightX;
  int versionTextY;

  int bookProgressBarHeight;
};

namespace BaseMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 15,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 20,
                                 .headerHeight = 45,
                                 .verticalSpacing = 10,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 30,
                                 .listWithSubtitleRowHeight = 65,
                                 .menuRowHeight = 36,
                                 .menuSpacing = 8,
                                 .tabSpacing = 10,
                                 .tabBarHeight = 50,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 20,
                                 .homeCoverHeight = 400,
                                 .homeCoverTileHeight = 400,
                                 .homeRecentBooksCount = 10,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .versionTextRightX = 20,
                                 .versionTextY = 738,
                                 .bookProgressBarHeight = 4};
}

class BaseTheme {
 public:
  static void invalidateHomeInfoStats();
  static void refreshHomeInfoStats();
  static uint64_t homeInfoStatsSignature();

  // Layout helpers (moved from UITheme)
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle);
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverHeight);

  // Component drawing methods
  void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) const;
  void drawBatteryLeft(const GfxRenderer& renderer, Rect rect,
                       bool showPercentage = true) const;
  void drawBatteryRight(const GfxRenderer& renderer, Rect rect,
                        bool showPercentage = true,
                        int textFont = SMALL_FONT_ID,
                        int iconWidth = BaseMetrics::values.batteryWidth,
                        int iconHeight = BaseMetrics::values.batteryHeight,
                        bool showIcon = true) const;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const;
  void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle,
                const std::function<std::string(int index)>& rowIcon,
                const std::function<std::string(int index)>& rowValue) const;

  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title) const;
  void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                  bool selected) const;
  BookListVisibility drawRecentBookCover(GfxRenderer& renderer, Rect rect,
                                         const std::vector<RecentBook>& recentBooks, int selectorIndex,
                                         int scrollOffset = 0) const;
  void drawRecentBookSingleCover(GfxRenderer& renderer, Rect rect,
                                  const std::vector<RecentBook>& recentBooks, int selectorIndex,
                                  int scrollOffset) const;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<std::string(int index)>& rowIcon) const;
  void drawHomeInfoStatsPopup(const GfxRenderer& renderer) const;
  Rect drawPopup(GfxRenderer& renderer, const char* message) const;
  void fillPopupProgress(GfxRenderer& renderer, const Rect& layout, const int progress) const;
  void drawReadingProgressBar(const GfxRenderer& renderer, const size_t bookProgress) const;
};

void drawDashedHLine(const GfxRenderer& renderer, int x, int y, int w, int thickness);

// Global theme instance — used by all UI drawing code via the GUI macro.
extern BaseTheme baseTheme;
#define GUI baseTheme
