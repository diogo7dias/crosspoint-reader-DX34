#include "BaseTheme.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "util/StringUtils.h"
#include "util/TransitionFeedback.h"

extern HalGPIO gpio;

#include "Battery.h"
#include "CrossPointSettings.h"
#include "I18n.h"
#include "RecentBooksStore.h"
#include "fontIds.h"
#include "util/BookProgress.h"

// Global theme instance (used by GUI macro in BaseTheme.h)
BaseTheme baseTheme;

// Internal constants
namespace {
constexpr int batteryPercentSpacing = 4;
constexpr int homeMenuMargin = 20;
constexpr int homeMarginTop = 30;

// Try to open a cover BMP for a book. Attempts the [HEIGHT]-templated thumb path
// at the requested height first, then at common pre-generated heights, then falls
// back to the full-size cover.bmp (replacing "thumb_[HEIGHT].bmp" with "cover.bmp").
// Returns true if file was opened successfully (caller must close it).
bool tryOpenCoverBmp(const std::string& coverBmpPath, int desiredHeight, FsFile& outFile) {
  if (coverBmpPath.empty()) return false;

  // 1. Try exact height thumb
  const std::string exactPath = BaseTheme::getCoverThumbPath(coverBmpPath, desiredHeight);
  if (Storage.exists(exactPath.c_str()) && Storage.openFileForRead("HOME", exactPath, outFile)) {
    return true;
  }

  // 2. Try common pre-generated thumb heights
  static const int commonHeights[] = {400, 300, 200, 600, 800};
  for (int h : commonHeights) {
    if (h == desiredHeight) continue;
    const std::string path = BaseTheme::getCoverThumbPath(coverBmpPath, h);
    if (Storage.exists(path.c_str()) && Storage.openFileForRead("HOME", path, outFile)) {
      return true;
    }
  }

  // 3. Try full-size cover.bmp (coverBmpPath has "/thumb_[HEIGHT].bmp", replace with "/cover.bmp")
  const size_t thumbPos = coverBmpPath.find("/thumb_[HEIGHT]");
  if (thumbPos != std::string::npos) {
    const std::string fullCoverPath = coverBmpPath.substr(0, thumbPos) + "/cover.bmp";
    if (Storage.exists(fullCoverPath.c_str()) && Storage.openFileForRead("HOME", fullCoverPath, outFile)) {
      return true;
    }
  }

  return false;
}

bool endsWithIgnoreCase(const char* text, const char* suffix) {
  if (!text || !suffix) return false;
  size_t textLen = 0;
  while (text[textLen] != '\0') textLen++;
  size_t suffixLen = 0;
  while (suffix[suffixLen] != '\0') suffixLen++;
  if (suffixLen > textLen) return false;

  const size_t start = textLen - suffixLen;
  for (size_t i = 0; i < suffixLen; ++i) {
    char a = text[start + i];
    char b = suffix[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + ('a' - 'A'));
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + ('a' - 'A'));
    if (a != b) return false;
  }
  return true;
}

bool isBookFile(const char* name) {
  return endsWithIgnoreCase(name, ".epub") || endsWithIgnoreCase(name, ".xtc") || endsWithIgnoreCase(name, ".xtch") ||
         endsWithIgnoreCase(name, ".txt");
}

bool isBmpFile(const char* name) { return endsWithIgnoreCase(name, ".bmp"); }

// Iterative directory scan — avoids deep recursion on ESP32 stack.
// Feeds watchdog every 32 entries to prevent WDT reset on large SD cards.
void countFilesIterative(FsFile& root, uint32_t& bookCount, uint32_t& bmpCount, bool countBooks, bool countBmps) {
  char name[256];
  uint32_t entryCount = 0;
  std::vector<FsFile> dirStack;
  dirStack.reserve(8);  // typical SD nesting depth; avoids realloc fragmentation
  dirStack.push_back(std::move(root));

  while (!dirStack.empty()) {
    FsFile& dir = dirStack.back();
    auto entry = dir.openNextFile();
    if (!entry) {
      dir.close();
      dirStack.pop_back();
      continue;
    }

    entry.getName(name, sizeof(name));
    if (entry.isDirectory()) {
      if (name[0] != '.') {
        dirStack.push_back(std::move(entry));
      } else {
        entry.close();
      }
      continue;
    }

    if (countBooks && isBookFile(name)) {
      ++bookCount;
    }
    if (countBmps && isBmpFile(name)) {
      ++bmpCount;
    }
    entry.close();

    if ((++entryCount & 0x1F) == 0) {
      esp_task_wdt_reset();
    }
  }
}

// Flat scan of a single directory (no recursion). Counts BMP files only.
uint32_t countBmpsInDir(const char* path) {
  auto dir = Storage.open(path);
  if (!dir || !dir.isDirectory()) return 0;

  uint32_t count = 0;
  uint32_t entryCount = 0;
  char name[256];
  for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    entry.getName(name, sizeof(name));
    if (!entry.isDirectory() && isBmpFile(name)) {
      ++count;
    }
    entry.close();
    if ((++entryCount & 0x1F) == 0) {
      esp_task_wdt_reset();
    }
  }
  dir.close();
  return count;
}

void drawDashedRect(const GfxRenderer& renderer, int x, int y, int w, int h) {
  constexpr int dash = 5;
  constexpr int gap = 3;
  constexpr int step = dash + gap;
  const int x2 = x + w - 1;
  const int y2 = y + h - 1;

  for (int px = x; px <= x2; px += step) {
    const int end = std::min(px + dash - 1, x2);
    renderer.drawLine(px, y, end, y);
    renderer.drawLine(px, y2, end, y2);
  }
  for (int py = y; py <= y2; py += step) {
    const int end = std::min(py + dash - 1, y2);
    renderer.drawLine(x, py, x, end);
    renderer.drawLine(x2, py, x2, end);
  }
}

std::string buildAuthorInitials(const std::string& author) {
  std::string initials;
  bool newWord = true;
  for (const char ch : author) {
    if (ch == ' ' || ch == '\t') {
      newWord = true;
      continue;
    }
    if (newWord) {
      if (ch >= 'a' && ch <= 'z') {
        initials.push_back(static_cast<char>(ch - ('a' - 'A')));
      } else {
        initials.push_back(ch);
      }
      if (initials.size() >= 4) {
        break;
      }
      newWord = false;
    }
  }
  return initials;
}

std::vector<std::string> wrapText(const GfxRenderer& renderer, const std::string& input, const int maxWidth) {
  std::vector<std::string> lines;
  if (input.empty()) {
    lines.push_back("");
    return lines;
  }

  size_t i = 0;
  while (i < input.size()) {
    while (i < input.size() && input[i] == ' ') {
      i++;
    }
    if (i >= input.size()) {
      break;
    }

    std::string line;
    size_t lineEndPos = i;
    while (lineEndPos < input.size()) {
      size_t wordEnd = lineEndPos;
      while (wordEnd < input.size() && input[wordEnd] != ' ') {
        wordEnd++;
      }
      const std::string word = input.substr(lineEndPos, wordEnd - lineEndPos);
      const std::string candidate = line.empty() ? word : (line + " " + word);

      if (renderer.getTextWidth(UI_10_FONT_ID, candidate.c_str()) <= maxWidth) {
        line = candidate;
        lineEndPos = wordEnd;
        while (lineEndPos < input.size() && input[lineEndPos] == ' ') {
          lineEndPos++;
        }
        continue;
      }

      if (line.empty()) {
        size_t fit = 1;
        while (fit < word.size() && renderer.getTextWidth(UI_10_FONT_ID, word.substr(0, fit + 1).c_str()) <= maxWidth) {
          fit++;
        }
        line = word.substr(0, fit);
        lineEndPos += fit;
      }
      break;
    }

    if (line.empty()) {
      line = renderer.truncatedText(UI_10_FONT_ID, input.substr(i).c_str(), maxWidth);
      lines.push_back(line);
      break;
    }

    lines.push_back(line);
    i = lineEndPos;
  }

  if (lines.empty()) {
    lines.push_back(renderer.truncatedText(UI_10_FONT_ID, input.c_str(), maxWidth));
  }
  return lines;
}

// Draw a centered "N more above/below" indicator badge
void drawMoreIndicator(const GfxRenderer& renderer, int count, const char* direction, int centerX, int centerW, int y,
                       int rowLineHeight) {
  const std::string text = std::to_string(count) + " more " + direction;
  const int textW = renderer.getTextWidth(UI_10_FONT_ID, text.c_str());
  const int badgeW = textW + 24;
  const int badgeH = rowLineHeight + 6;
  const int badgeX = centerX + (centerW - badgeW) / 2;
  renderer.fillRect(badgeX, y, badgeW, badgeH);
  const int textX = badgeX + (badgeW - textW) / 2;
  renderer.drawText(UI_10_FONT_ID, textX, y + 3, text.c_str(), false);
}

struct HomeInfoStats {
  uint32_t bookCount = 0;
  uint32_t sleepBmpCount = 0;
  uint32_t sleepFavoriteCount = 0;
  uint32_t sleepPauseCount = 0;
  uint64_t freeBytes = 0;
  bool valid = false;
};

HomeInfoStats gHomeInfoStats;

void scanHomeInfoStats(HomeInfoStats& stats) {
  stats.bookCount = 0;
  stats.sleepBmpCount = 0;
  stats.sleepFavoriteCount = 0;
  stats.sleepPauseCount = 0;
  stats.freeBytes = Storage.freeBytes();

  // Count books recursively from root (skip hidden dirs)
  auto root = Storage.open("/");
  if (root && root.isDirectory()) {
    countFilesIterative(root, stats.bookCount, stats.sleepBmpCount, true, false);
    // root closed by countFilesIterative
  }

  // Count sleep BMPs + favorites in /sleep (flat scan).
  // Favorite detection uses suffix-only check ("_F.bmp") — zero heap allocations.
  {
    auto dir = Storage.open("/sleep");
    if (dir && dir.isDirectory()) {
      uint32_t entryCount = 0;
      char name[256];
      for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
        entry.getName(name, sizeof(name));
        if (!entry.isDirectory() && isBmpFile(name)) {
          ++stats.sleepBmpCount;
          if (endsWithIgnoreCase(name, "_F.bmp")) {
            ++stats.sleepFavoriteCount;
          }
        }
        entry.close();
        if ((++entryCount & 0x1F) == 0) {
          esp_task_wdt_reset();
        }
      }
      dir.close();
    }
  }

  // Count BMPs in /sleep pause
  stats.sleepPauseCount = countBmpsInDir("/sleep pause");

  stats.valid = true;
}

const HomeInfoStats& getHomeInfoStats() {
  if (gHomeInfoStats.valid) {
    return gHomeInfoStats;
  }

  scanHomeInfoStats(gHomeInfoStats);
  return gHomeInfoStats;
}

// Helper: draw battery icon at given position
void drawBatteryIcon(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight, uint16_t percentage) {
  // Top line
  renderer.drawLine(x + 1, y, x + battWidth - 3, y);
  // Bottom line
  renderer.drawLine(x + 1, y + rectHeight - 1, x + battWidth - 3, y + rectHeight - 1);
  // Left line
  renderer.drawLine(x, y + 1, x, y + rectHeight - 2);
  // Battery end
  renderer.drawLine(x + battWidth - 2, y + 1, x + battWidth - 2, y + rectHeight - 2);
  renderer.drawPixel(x + battWidth - 1, y + 3);
  renderer.drawPixel(x + battWidth - 1, y + rectHeight - 4);
  renderer.drawLine(x + battWidth - 0, y + 4, x + battWidth - 0, y + rectHeight - 5);

  const bool charging = gpio.isUsbConnected();

  // The +1 is to round up, so that we always fill at least one pixel
  const int maxFillWidth = battWidth - 5;
  const int fillHeight = rectHeight - 4;
  if (maxFillWidth <= 0 || fillHeight <= 0) {
    return;
  }
  int filledWidth = percentage * maxFillWidth / 100 + 1;
  if (filledWidth > maxFillWidth) {
    filledWidth = maxFillWidth;
  }

  // When charging, ensure minimum fill so lightning bolt is fully visible
  constexpr int minFillForBolt = 8;
  if (charging && filledWidth < minFillForBolt) {
    filledWidth = std::min(minFillForBolt, maxFillWidth);
  }

  renderer.fillRect(x + 2, y + 2, filledWidth, fillHeight);

  // Draw lightning bolt when charging (white/inverted on black fill for visibility)
  if (charging) {
    const int boltX = x + 4;
    const int boltY = y + 2;
    renderer.drawLine(boltX + 4, boltY + 0, boltX + 5, boltY + 0, false);
    renderer.drawLine(boltX + 3, boltY + 1, boltX + 4, boltY + 1, false);
    renderer.drawLine(boltX + 2, boltY + 2, boltX + 5, boltY + 2, false);
    renderer.drawLine(boltX + 3, boltY + 3, boltX + 4, boltY + 3, false);
    renderer.drawLine(boltX + 2, boltY + 4, boltX + 3, boltY + 4, false);
    renderer.drawLine(boltX + 1, boltY + 5, boltX + 4, boltY + 5, false);
    renderer.drawLine(boltX + 2, boltY + 6, boltX + 3, boltY + 6, false);
    renderer.drawLine(boltX + 1, boltY + 7, boltX + 2, boltY + 7, false);
  }
}
}  // namespace

// Declared in BaseTheme.h at global scope; kept out of the anonymous
// namespace above so external TUs (e.g. EpubReaderActivity) can link
// against it.
void drawDashedHLine(const GfxRenderer& renderer, int x, int y, int w, int thickness) {
  constexpr int dash = 8;
  constexpr int gap = 4;
  constexpr int step = dash + gap;
  const int x2 = x + w - 1;
  for (int px = x; px <= x2; px += step) {
    const int segW = std::min(dash, x2 - px + 1);
    renderer.fillRect(px, y, segW, thickness, true);
  }
}

void BaseTheme::invalidateHomeInfoStats() { gHomeInfoStats.valid = false; }

void BaseTheme::refreshHomeInfoStats() {
  gHomeInfoStats.valid = false;
  scanHomeInfoStats(gHomeInfoStats);
}

uint64_t BaseTheme::homeInfoStatsSignature() {
  const auto& stats = getHomeInfoStats();
  // Compact signature from the three displayed values.
  return (static_cast<uint64_t>(stats.bookCount) << 40) ^ (static_cast<uint64_t>(stats.sleepBmpCount) << 24) ^
         stats.freeBytes;
}

void BaseTheme::drawBatteryLeft(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Reader/status usage is text-only for a cleaner, more compact layout.
  if (!showPercentage) {
    return;
  }
  const uint16_t percentage = battery.readPercentage();
  const auto percentageText = std::to_string(percentage) + "%";
  const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
  const int textY = rect.y + std::max(0, (rect.height - textHeight) / 2);
  renderer.drawText(SMALL_FONT_ID, rect.x, textY, percentageText.c_str());
}

void BaseTheme::drawBatteryRight(const GfxRenderer& renderer, Rect rect, const bool showPercentage, const int textFont,
                                 const int iconWidth, const int iconHeight, const bool showIcon) const {
  // Right aligned: percentage on left, icon on right (UI headers)
  // rect.x is positioned for the icon, or the right text edge when icon is hidden.
  const uint16_t percentage = battery.readPercentage();
  const int y = rect.y + std::max(0, (rect.height - iconHeight) / 2);

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    const int textWidth = renderer.getTextWidth(textFont, percentageText.c_str());
    const int textHeight = renderer.getTextHeight(textFont);
    const int textX = showIcon ? (rect.x - textWidth - batteryPercentSpacing) : (rect.x - textWidth);
    const int textY = rect.y + std::max(0, (rect.height - textHeight) / 2);
    // Clear the area where we're going to draw the text to prevent ghosting.
    renderer.fillRect(textX, textY, textWidth, textHeight, false);
    // Draw text to the left of the icon
    renderer.drawText(textFont, textX, textY, percentageText.c_str());
  }

  if (showIcon) {
    drawBatteryIcon(renderer, rect.x, y, iconWidth, iconHeight, percentage);
  }
}

void BaseTheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, const size_t current,
                                const size_t total) const {
  if (total == 0) {
    return;
  }

  // Use 64-bit arithmetic to avoid overflow for large files
  const int percent = static_cast<int>((static_cast<uint64_t>(current) * 100) / total);

  // Draw outline
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  // Draw filled portion
  const int fillWidth = (rect.width - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(rect.x + 2, rect.y + 2, fillWidth, rect.height - 4);
  }

  // Draw percentage text centered below bar
  const std::string percentText = std::to_string(percent) + "%";
  renderer.drawCenteredText(UI_10_FONT_ID, rect.y + rect.height + 15, percentText.c_str());
}

void BaseTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonHeight = BaseMetrics::values.buttonHintsHeight;
  constexpr int buttonY = BaseMetrics::values.buttonHintsHeight;
  constexpr int textYOffset = 7;
  constexpr int padX = 10;        // horizontal padding inside button
  constexpr int marginX = 20;     // margin from screen edges
  constexpr int minButtonW = 60;  // minimum button width

  const char* labels[] = {btn1, btn2, btn3, btn4};

  // Measure the text width each button needs
  int textWidths[4] = {};
  int activeCount = 0;
  for (int i = 0; i < 4; i++) {
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const char* sep = strchr(labels[i], '\n');
      if (sep) {
        const std::string mainPart(labels[i], sep - labels[i]);
        const int mainW = renderer.getTextWidth(UI_10_FONT_ID, mainPart.c_str());
        const int holdW = renderer.getTextWidth(SMALL_FONT_ID, sep + 1);
        textWidths[i] = mainW + 2 + holdW;
      } else {
        textWidths[i] = renderer.getTextWidth(UI_10_FONT_ID, labels[i]);
      }
      activeCount++;
    }
  }

  if (activeCount == 0) {
    renderer.setOrientation(orig_orientation);
    return;
  }

  // Compute button widths: text + padding, with minimum
  int buttonWidths[4] = {};
  for (int i = 0; i < 4; i++) {
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      buttonWidths[i] = std::max(minButtonW, textWidths[i] + padX * 2);
    }
  }

  // Each physical button owns a fixed slot across the screen width.
  // Buttons are content-sized and centered within their slot.
  const int availableW = pageWidth - marginX * 2;
  const int slotW = availableW / 4;

  for (int i = 0; i < 4; i++) {
    if (labels[i] == nullptr || labels[i][0] == '\0') continue;

    const int slotX = marginX + i * slotW;
    const int bw = buttonWidths[i];
    const int x = slotX + (slotW - bw) / 2;  // center button in slot
    renderer.fillRect(x, pageHeight - buttonY, bw, buttonHeight, false);
    renderer.drawRect(x, pageHeight - buttonY, bw, buttonHeight);

    const char* sep = strchr(labels[i], '\n');
    if (sep) {
      const std::string mainLabel(labels[i], sep - labels[i]);
      const char* holdLabel = sep + 1;
      const int mainW = renderer.getTextWidth(UI_10_FONT_ID, mainLabel.c_str());
      const int holdW = renderer.getTextWidth(SMALL_FONT_ID, holdLabel);
      const int totalW = mainW + 2 + holdW;
      const int startX = x + (bw - totalW) / 2;
      renderer.drawText(UI_10_FONT_ID, startX, pageHeight - buttonY + textYOffset, mainLabel.c_str());
      renderer.drawText(SMALL_FONT_ID, startX + mainW + 2, pageHeight - buttonY + textYOffset + 3, holdLabel);
    } else {
      const int tw = textWidths[i];
      const int textX = x + (bw - tw) / 2;
      renderer.drawText(UI_10_FONT_ID, textX, pageHeight - buttonY + textYOffset, labels[i]);
    }
  }

  renderer.setOrientation(orig_orientation);
}

void BaseTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = BaseMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 80;                                       // Height on screen (width when rotated)
  constexpr int buttonX = 4;                                             // Distance from right edge
  // Position for the button group - buttons share a border so they're adjacent
  constexpr int topButtonY = 345;  // Top button position

  const char* labels[] = {topBtn, bottomBtn};

  // Draw the shared border for both buttons as one unit
  const int x = screenWidth - buttonX - buttonWidth;

  // Draw top button outline (3 sides, bottom open)
  if (topBtn != nullptr && topBtn[0] != '\0') {
    renderer.drawLine(x, topButtonY, x + buttonWidth - 1, topButtonY);                                       // Top
    renderer.drawLine(x, topButtonY, x, topButtonY + buttonHeight - 1);                                      // Left
    renderer.drawLine(x + buttonWidth - 1, topButtonY, x + buttonWidth - 1, topButtonY + buttonHeight - 1);  // Right
  }

  // Draw shared middle border
  if ((topBtn != nullptr && topBtn[0] != '\0') || (bottomBtn != nullptr && bottomBtn[0] != '\0')) {
    renderer.drawLine(x, topButtonY + buttonHeight, x + buttonWidth - 1, topButtonY + buttonHeight);  // Shared border
  }

  // Draw bottom button outline (3 sides, top is shared)
  if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
    renderer.drawLine(x, topButtonY + buttonHeight, x, topButtonY + 2 * buttonHeight - 1);  // Left
    renderer.drawLine(x + buttonWidth - 1, topButtonY + buttonHeight, x + buttonWidth - 1,
                      topButtonY + 2 * buttonHeight - 1);  // Right
    renderer.drawLine(x, topButtonY + 2 * buttonHeight - 1, x + buttonWidth - 1,
                      topButtonY + 2 * buttonHeight - 1);  // Bottom
  }

  // Draw text for each button
  for (int i = 0; i < 2; i++) {
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int y = topButtonY + i * buttonHeight;

      // Draw rotated text centered in the button
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
      const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);

      // Center the rotated text in the button
      const int textX = x + (buttonWidth - textHeight) / 2;
      const int textY = y + (buttonHeight + textWidth) / 2;

      renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, labels[i]);
    }
  }
}

void BaseTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<std::string(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue) const {
  int rowHeight =
      (rowSubtitle != nullptr) ? BaseMetrics::values.listWithSubtitleRowHeight : BaseMetrics::values.listRowHeight;
  int pageItems = rect.height / rowHeight;
  if (pageItems < 1) {
    pageItems = 1;
  }

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    constexpr int indicatorWidth = 20;
    constexpr int arrowSize = 6;
    constexpr int margin = 15;  // Offset from right edge

    const int centerX = rect.x + rect.width - indicatorWidth / 2 - margin;
    const int indicatorTop = rect.y;  // Offset to avoid overlapping side button hints
    const int indicatorBottom = rect.y + rect.height - arrowSize;

    // Draw up arrow at top (^) - narrow point at top, wide base at bottom
    for (int i = 0; i < arrowSize; ++i) {
      const int lineWidth = 1 + i * 2;
      const int startX = centerX - i;
      renderer.drawLine(startX, indicatorTop + i, startX + lineWidth - 1, indicatorTop + i);
    }

    // Draw down arrow at bottom (v) - wide base at top, narrow point at bottom
    for (int i = 0; i < arrowSize; ++i) {
      const int lineWidth = 1 + (arrowSize - 1 - i) * 2;
      const int startX = centerX - (arrowSize - 1 - i);
      renderer.drawLine(startX, indicatorBottom - arrowSize + 1 + i, startX + lineWidth - 1,
                        indicatorBottom - arrowSize + 1 + i);
    }
  }

  // Draw selection
  int contentWidth = rect.width - 5;
  if (selectedIndex >= 0) {
    renderer.fillRect(0, rect.y + selectedIndex % pageItems * rowHeight - 2, rect.width, rowHeight);
  }
  // Draw all items
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;
    int textWidth = contentWidth - BaseMetrics::values.contentSidePadding * 2 - (rowValue != nullptr ? 60 : 0);

    // Draw name
    auto itemName = rowTitle(i);
    auto font = (rowSubtitle != nullptr) ? UI_12_FONT_ID : UI_10_FONT_ID;
    auto item = renderer.truncatedText(font, itemName.c_str(), textWidth);
    renderer.drawText(font, rect.x + BaseMetrics::values.contentSidePadding, itemY, item.c_str(), i != selectedIndex);

    if (rowSubtitle != nullptr) {
      // Draw subtitle
      std::string subtitleText = rowSubtitle(i);
      auto subtitle = renderer.truncatedText(UI_10_FONT_ID, subtitleText.c_str(), textWidth);
      renderer.drawText(UI_10_FONT_ID, rect.x + BaseMetrics::values.contentSidePadding, itemY + 30, subtitle.c_str(),
                        i != selectedIndex);
    }

    if (rowValue != nullptr) {
      // Draw value as a compact badge (black background, white text)
      std::string valueText = rowValue(i);
      if (!valueText.empty()) {
        const int valueTextWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
        const int valueTextHeight = renderer.getLineHeight(UI_10_FONT_ID);
        constexpr int badgePadX = 4;
        const int badgeWidth = valueTextWidth + badgePadX * 2;
        const int badgeX = rect.x + contentWidth - BaseMetrics::values.contentSidePadding - badgeWidth;
        const int badgeY = itemY;

        renderer.fillRect(badgeX, badgeY, badgeWidth, valueTextHeight, true);
        renderer.drawText(UI_10_FONT_ID, badgeX + badgePadX, badgeY, valueText.c_str(), false);
      }
    }
  }
}

void BaseTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title) const {
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const bool homeStyleHeader = title == nullptr;
  const int batteryTextFont = UI_10_FONT_ID;
  const int batteryIconWidth =
      homeStyleHeader ? BaseMetrics::values.batteryWidth + 2 : BaseMetrics::values.batteryWidth;
  const int batteryIconHeight =
      homeStyleHeader ? BaseMetrics::values.batteryHeight + 2 : BaseMetrics::values.batteryHeight;
  const bool showBatteryIcon = false;
  // Align percentage text to the right edge.
  const int batteryX = rect.x + rect.width - 12 - (showBatteryIcon ? batteryIconWidth : 0);
  drawBatteryRight(renderer, Rect{batteryX, rect.y + 5, batteryIconWidth, batteryIconHeight}, showBatteryPercentage,
                   batteryTextFont, batteryIconWidth, batteryIconHeight, showBatteryIcon);

  if (title) {
    const int padding = 12 + (showBatteryPercentage ? renderer.getTextWidth(batteryTextFont, "100%") : 0);
    auto truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, title,
                                                 rect.width - padding * 2 - BaseMetrics::values.contentSidePadding * 2,
                                                 EpdFontFamily::REGULAR);
    renderer.drawCenteredText(UI_12_FONT_ID, rect.y + 5, truncatedTitle.c_str(), true, EpdFontFamily::REGULAR);
  }
}

void BaseTheme::drawTabBar(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                           bool selected) const {
  constexpr int underlineHeight = 2;  // Height of selection underline
  constexpr int underlineGap = 4;     // Gap between text and underline

  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);

  int currentX = rect.x + BaseMetrics::values.contentSidePadding;

  for (const auto& tab : tabs) {
    const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, tab.label, EpdFontFamily::REGULAR);

    // Draw underline for selected tab
    if (tab.selected) {
      if (selected) {
        renderer.fillRect(currentX - 3, rect.y, textWidth + 6, lineHeight + underlineGap);
      } else {
        renderer.fillRect(currentX, rect.y + lineHeight + underlineGap, textWidth, underlineHeight);
      }
    }

    // Draw tab label
    renderer.drawText(UI_12_FONT_ID, currentX, rect.y, tab.label, !(tab.selected && selected), EpdFontFamily::REGULAR);

    currentX += textWidth + BaseMetrics::values.tabSpacing;
  }
}

// Draw the "Recent Book" cover card on the home screen
BookListVisibility BaseTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect,
                                                  const std::vector<RecentBook>& recentBooks, int selectorIndex,
                                                  int scrollOffset) const {
  const int maxRowsCap = std::max(1, BaseMetrics::values.homeRecentBooksCount);
  const int count = std::min(static_cast<int>(recentBooks.size()), maxRowsCap);
  constexpr int maxVisibleBooks = 8;
  // Clamp scrollOffset to valid range
  const int clampedOffset = std::max(0, std::min(scrollOffset, std::max(0, count - 1)));
  constexpr int rowGap = 4;
  const int rowLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  constexpr int rowsTopInset = 18;
  constexpr int rowsBottomInset = 6;
  const int rowsTopMinY = rect.y + rowsTopInset;
  const int rowsBottomY = rect.y + rect.height - rowsBottomInset;
  const int rowsAvailableHeight = rowsBottomY - rowsTopMinY;
  const int availableRowW = std::max(1, rect.width - BaseMetrics::values.contentSidePadding * 2);
  constexpr int maxRowW = 520;
  const int rowW = std::min(availableRowW, maxRowW);
  const int rowX = rect.x + (rect.width - rowW) / 2;
  const int contentX = rowX + 12;
  const int contentW = std::max(1, rowW - 24);

  // Indicator zone height (always reserved when list can scroll)
  const int indicatorH = rowLineHeight + 8;
  const bool reserveIndicators = count > 1;
  const int aboveIndicatorH = reserveIndicators ? indicatorH : 0;
  const int belowIndicatorH = reserveIndicators ? indicatorH : 0;

  // Content zone: fixed area between the two indicator zones
  const int effectiveTopY = rowsTopMinY + aboveIndicatorH;
  const int effectiveBottomY = rowsBottomY - belowIndicatorH;
  const int contentHeight = effectiveBottomY - effectiveTopY;

  if (rowsAvailableHeight <= 0 || contentHeight <= 0 || count == 0) {
    return {0, 0, count};
  }

  // Helper: measure a book's wrapped text and height
  struct BookEntry {
    int bookIdx;
    std::vector<std::string> lines;
    int height;
  };
  auto measureBook = [&](int idx) -> BookEntry {
    const std::string initials = buildAuthorInitials(recentBooks[idx].author);
    const std::string rowText =
        initials.empty() ? recentBooks[idx].title : (recentBooks[idx].title + " by " + initials);
    auto lines = wrapText(renderer, rowText, contentW);
    const int h = static_cast<int>(lines.size()) * rowLineHeight + 6;
    return {idx, std::move(lines), h};
  };

  // Helper: build visible entries from a start offset
  auto buildVisibleEntries = [&](int startIdx) {
    std::vector<BookEntry> entries;
    int accumulated = 0;
    for (int i = startIdx; i < count && static_cast<int>(entries.size()) < maxVisibleBooks; i++) {
      auto entry = measureBook(i);
      const int needed = accumulated + (entries.empty() ? 0 : rowGap) + entry.height;
      if (needed > contentHeight) break;
      accumulated = needed;
      entries.push_back(std::move(entry));
    }
    return entries;
  };

  // Phase 1: Build visible entries from scrollOffset
  std::vector<BookEntry> visibleEntries = buildVisibleEntries(clampedOffset);

  // Phase 2: If the selected book isn't visible, adjust scroll offset
  // This handles the case where the next book is taller than the space
  // freed by scrolling one book off the top.
  if (selectorIndex >= 0 && selectorIndex < count && !visibleEntries.empty()) {
    const int lastVisible = visibleEntries.back().bookIdx;
    if (selectorIndex > lastVisible) {
      // Selected book is below visible range.
      // Work backwards from the selected book to find the best start offset
      // so the selected book appears at the bottom with as many above as fit.
      auto selectedEntry = measureBook(selectorIndex);
      int totalH = selectedEntry.height;
      int newOffset = selectorIndex;
      for (int i = selectorIndex - 1; i >= 0; i--) {
        const int h = measureBook(i).height;
        if (totalH + rowGap + h > contentHeight) break;
        totalH += rowGap + h;
        newOffset = i;
      }
      visibleEntries = buildVisibleEntries(newOffset);
    }
  }

  // Edge case: if zero books fit (very tall title), force-include the first one
  if (visibleEntries.empty() && clampedOffset < count) {
    visibleEntries.push_back(measureBook(clampedOffset));
  }

  const int firstVisible = visibleEntries.front().bookIdx;
  const int lastVisible = visibleEntries.back().bookIdx;
  const bool hasMoreAbove = firstVisible > 0;
  const bool hasMoreBelow = lastVisible < count - 1;

  // Phase 3: Draw

  // Compute total height of visible entries for centering
  int totalVisibleHeight = 0;
  for (size_t i = 0; i < visibleEntries.size(); i++) {
    totalVisibleHeight += visibleEntries[i].height;
    if (i > 0) totalVisibleHeight += rowGap;
  }

  // Center content vertically within the content zone
  int rowY = effectiveTopY;
  if (totalVisibleHeight < contentHeight) {
    rowY += (contentHeight - totalVisibleHeight) / 2;
  }

  // Draw "N more above" indicator
  if (hasMoreAbove) {
    drawMoreIndicator(renderer, firstVisible, "above", rowX, rowW, rowsTopMinY, rowLineHeight);
  }

  // Draw visible books
  for (const auto& entry : visibleEntries) {
    const bool selected = (selectorIndex == entry.bookIdx);
    const bool textBlack = !selected;

    if (selected) {
      renderer.fillRect(rowX, rowY, rowW, entry.height, true);
    }

    int baselineY = rowY + 3;
    for (const auto& line : entry.lines) {
      renderer.drawText(UI_10_FONT_ID, contentX, baselineY, line.c_str(), textBlack);
      baselineY += rowLineHeight;
    }

    rowY += entry.height + rowGap;
  }

  // Draw "N more below" indicator
  if (hasMoreBelow) {
    drawMoreIndicator(renderer, count - lastVisible - 1, "below", rowX, rowW, effectiveBottomY + 2, rowLineHeight);
  }

  return {firstVisible, lastVisible, count};
}

void BaseTheme::drawRecentBookSingleCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                          int selectorIndex, int scrollOffset) const {
  const int count = static_cast<int>(recentBooks.size());
  if (count == 0) return;

  const int clampedIdx = std::max(0, std::min(scrollOffset, count - 1));
  const int rowLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int indicatorH = rowLineHeight + 8;
  const bool hasMoreAbove = clampedIdx > 0;
  const bool hasMoreBelow = clampedIdx < count - 1;

  // Reserve space for indicators
  const int aboveH = hasMoreAbove ? indicatorH : 0;
  const int belowH = hasMoreBelow ? indicatorH : 0;

  // Layout: indicators at top/bottom, cover + text info in between
  const int contentTop = rect.y + aboveH + 2;
  const int contentBottom = rect.y + rect.height - belowH - 2;
  const int contentH = contentBottom - contentTop;

  // Space for title + author text below cover
  const int titleLineH = rowLineHeight;
  const int authorLineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int textBlockH = titleLineH + authorLineH + 6;

  const RecentBook& book = recentBooks[clampedIdx];

  // Determine cover size from actual image dimensions (or default 2:3 if no cover)
  const int coverMaxH = std::max(20, contentH - textBlockH - 4);
  const int coverMaxW = rect.width - 40;
  int imgW = 2, imgH = 3;  // default aspect ratio for placeholder

  // Try to open cover to get real dimensions
  FsFile coverFile;
  bool hasCoverFile = tryOpenCoverBmp(book.coverBmpPath, 400, coverFile);
  Bitmap* coverBitmap = nullptr;
  Bitmap bitmapStorage(coverFile, true);
  if (hasCoverFile) {
    if (bitmapStorage.parseHeaders() == BmpReaderError::Ok) {
      imgW = bitmapStorage.getWidth();
      imgH = bitmapStorage.getHeight();
      coverBitmap = &bitmapStorage;
    } else {
      coverFile.close();
      hasCoverFile = false;
    }
  }

  // Fit cover within available space preserving aspect ratio.
  // When we have a real bitmap, mirror drawBitmap's internal scaling so the
  // border matches the rendered image exactly.  For the placeholder (no cover),
  // use ratio-based sizing to fill the available space.
  int coverW, coverH;
  if (coverBitmap) {
    float scale = 1.0f;
    if (imgW > coverMaxW) {
      scale = static_cast<float>(coverMaxW) / static_cast<float>(imgW);
    }
    if (static_cast<int>(imgH * scale) > coverMaxH) {
      scale = static_cast<float>(coverMaxH) / static_cast<float>(imgH);
    }
    coverW = static_cast<int>(imgW * scale);
    coverH = static_cast<int>(imgH * scale);
  } else {
    const float imgRatio = static_cast<float>(imgW) / static_cast<float>(imgH);
    if (static_cast<int>(coverMaxH * imgRatio) <= coverMaxW) {
      coverH = coverMaxH;
      coverW = static_cast<int>(coverH * imgRatio);
    } else {
      coverW = coverMaxW;
      coverH = static_cast<int>(coverW / imgRatio);
    }
  }
  const int coverX = rect.x + (rect.width - coverW) / 2;
  const int coverY = contentTop + (coverMaxH - coverH) / 2;

  // Draw cover
  bool coverDrawn = false;
  if (coverBitmap) {
    renderer.drawBitmap(bitmapStorage, coverX, coverY, coverW, coverH);
    coverDrawn = true;
  }
  if (hasCoverFile) {
    coverFile.close();
  }

  // Fallback: plain gray rectangle with "no cover found" badge
  if (!coverDrawn) {
    renderer.fillRectDither(coverX, coverY, coverW, coverH, Color::LightGray);
    // "no cover found" badge centered in cover (same style as percentage badge)
    const char* noCoverText = "no cover found.";
    const int ncTextW = renderer.getTextWidth(UI_10_FONT_ID, noCoverText);
    const int ncBadgeW = ncTextW + 12;
    const int ncBadgeH = rowLineHeight + 4;
    const int ncBadgeX = coverX + (coverW - ncBadgeW) / 2;
    const int ncBadgeY = coverY + (coverH - ncBadgeH) / 2;
    renderer.fillRect(ncBadgeX, ncBadgeY, ncBadgeW, ncBadgeH);
    const int ncTextX = ncBadgeX + (ncBadgeW - ncTextW) / 2;
    renderer.drawText(UI_10_FONT_ID, ncTextX, ncBadgeY + 2, noCoverText, false);
  }

  // Draw cover border
  renderer.drawRect(coverX, coverY, coverW, coverH);

  // Draw percentage badge at bottom center of cover (black bg, white text)
  {
    const auto percent = BookProgress::getPercent(book.path);
    const std::string pctText = "[" + (percent.has_value() ? std::to_string(percent.value()) : "0") + "%]";
    const int pctTextW = renderer.getTextWidth(UI_10_FONT_ID, pctText.c_str());
    const int badgeW = pctTextW + 12;
    const int badgeH = rowLineHeight + 4;
    const int badgeX = coverX + (coverW - badgeW) / 2;
    const int badgeY = coverY + coverH - badgeH - 4;
    renderer.fillRect(badgeX, badgeY, badgeW, badgeH);
    const int pctTextX = badgeX + (badgeW - pctTextW) / 2;
    renderer.drawText(UI_10_FONT_ID, pctTextX, badgeY + 2, pctText.c_str(), false);
  }

  // Title below cover (bold, centered)
  const int textY = coverY + coverH + 4;
  const int textMaxW = rect.width - BaseMetrics::values.contentSidePadding * 2;
  {
    const std::string truncTitle =
        renderer.truncatedText(UI_10_FONT_ID, book.title.c_str(), textMaxW, EpdFontFamily::BOLD);
    const int titleW = renderer.getTextWidth(UI_10_FONT_ID, truncTitle.c_str(), EpdFontFamily::BOLD);
    const int titleX = rect.x + (rect.width - titleW) / 2;
    renderer.drawText(UI_10_FONT_ID, titleX, textY, truncTitle.c_str(), true, EpdFontFamily::BOLD);
  }

  // Author below title (smaller font, centered)
  if (!book.author.empty()) {
    const int authorY = textY + titleLineH + 2;
    const std::string truncAuthor = renderer.truncatedText(SMALL_FONT_ID, book.author.c_str(), textMaxW);
    const int authorW = renderer.getTextWidth(SMALL_FONT_ID, truncAuthor.c_str());
    const int authorX = rect.x + (rect.width - authorW) / 2;
    renderer.drawText(SMALL_FONT_ID, authorX, authorY, truncAuthor.c_str());
  }

  // "N more above" indicator
  if (hasMoreAbove) {
    drawMoreIndicator(renderer, clampedIdx, "above", rect.x, rect.width, rect.y + 2, rowLineHeight);
  }

  // "N more below" indicator
  if (hasMoreBelow) {
    const int belowBarH = rowLineHeight + 6;
    const int belowY = rect.y + rect.height - belowBarH - 2;
    drawMoreIndicator(renderer, count - clampedIdx - 1, "below", rect.x, rect.width, belowY, rowLineHeight);
  }
}

void BaseTheme::drawHomeInfoStatsPopup(const GfxRenderer& renderer) const {
  const auto& stats = getHomeInfoStats();

  const int popupW = std::min(renderer.getScreenWidth() - 20, 460);
  constexpr int textPadY = 10;
  constexpr int lineGap = 6;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineStep = lineHeight + lineGap;
  constexpr int statLines = 5;
  const int popupH = textPadY * 2 + lineStep * statLines;
  const int popupX = (renderer.getScreenWidth() - popupW) / 2;
  const int popupY = (renderer.getScreenHeight() - popupH) / 2;
  const int textX = popupX + 12;
  const int textMaxWidth = popupW - 24;

  renderer.fillRect(popupX - 2, popupY - 2, popupW + 4, popupH + 4, true);
  renderer.fillRect(popupX, popupY, popupW, popupH, false);
  drawDashedRect(renderer, popupX, popupY, popupW, popupH);

  const uint64_t gbScale = 1024ull * 1024ull * 1024ull;
  const uint64_t freeTenthsGb = (stats.freeBytes * 10ull) / gbScale;

  auto drawStatLine = [&](const int y, const char* labelText, const std::string& valueRegular) {
    const std::string labelWithSeparator = std::string(labelText) + "   ";
    const std::string label =
        renderer.truncatedText(UI_12_FONT_ID, labelWithSeparator.c_str(), textMaxWidth, EpdFontFamily::REGULAR);
    const int labelWidth = renderer.getTextWidth(UI_12_FONT_ID, label.c_str(), EpdFontFamily::REGULAR);

    renderer.drawText(UI_12_FONT_ID, textX, y, label.c_str(), true, EpdFontFamily::REGULAR);

    const int remaining = textMaxWidth - labelWidth;
    if (remaining <= 0) return;

    const std::string valueBracketed = "[" + valueRegular + "]";
    const std::string value = renderer.truncatedText(UI_12_FONT_ID, valueBracketed.c_str(), remaining);
    renderer.drawText(UI_12_FONT_ID, textX + labelWidth, y, value.c_str(), true, EpdFontFamily::REGULAR);
  };

  const int textY = popupY + textPadY;
  drawStatLine(textY, "BOOKS", std::to_string(stats.bookCount));
  drawStatLine(textY + lineStep, "SLEEP IMAGES", std::to_string(stats.sleepBmpCount));
  drawStatLine(textY + lineStep * 2, "SLEEP FAVORITES", std::to_string(stats.sleepFavoriteCount));
  drawStatLine(textY + lineStep * 3, "SLEEP PAUSE IMAGES", std::to_string(stats.sleepPauseCount));
  drawStatLine(textY + lineStep * 4, "FREE SD SPACE",
               std::to_string(freeTenthsGb / 10ull) + "." + std::to_string(freeTenthsGb % 10ull) + " GB");
}

void BaseTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<std::string(int index)>& rowIcon) const {
  for (int i = 0; i < buttonCount; ++i) {
    const int tileY = BaseMetrics::values.verticalSpacing + rect.y +
                      static_cast<int>(i) * (BaseMetrics::values.menuRowHeight + BaseMetrics::values.menuSpacing);

    const bool selected = selectedIndex == i;

    if (selected) {
      renderer.fillRect(rect.x + BaseMetrics::values.contentSidePadding, tileY,
                        rect.width - BaseMetrics::values.contentSidePadding * 2, BaseMetrics::values.menuRowHeight);
    } else {
      renderer.drawRect(rect.x + BaseMetrics::values.contentSidePadding, tileY,
                        rect.width - BaseMetrics::values.contentSidePadding * 2, BaseMetrics::values.menuRowHeight);
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
    const int textX = rect.x + (rect.width - textWidth) / 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int textY =
        tileY + (BaseMetrics::values.menuRowHeight - lineHeight) / 2;  // vertically centered assuming y is top of text
    // Invert text when the tile is selected, to contrast with the filled background
    renderer.drawText(UI_10_FONT_ID, textX, textY, label, selectedIndex != i);
  }
}

Rect BaseTheme::drawPopup(GfxRenderer& renderer, const char* message) const {
  // Delegate to TransitionFeedback for consistent positioning and stacking.
  TransitionFeedback::show(renderer, message);

  // Compute the rect that was drawn so callers (e.g. fillPopupProgress) can
  // reference it.  Must mirror the layout constants in TransitionFeedback::show.
  const std::string upper = StringUtils::toUpperAscii(message);
  constexpr int paddingX = 20;
  constexpr int paddingY = 12;
  constexpr int border = 2;
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, upper.c_str(), EpdFontFamily::REGULAR);
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int w = textWidth + paddingX * 2;
  const int h = textHeight + paddingY * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;
  // bottomY is past the border, so the box top is bottomY - h - border.
  const int y = TransitionFeedback::bottomY() - h - border;
  return Rect{x, y, w, h};
}

void BaseTheme::fillPopupProgress(GfxRenderer& renderer, const Rect& layout, const int progress) const {
  constexpr int barHeight = 4;
  const int barWidth = layout.width - 30;  // twice the margin in drawPopup to match text width
  const int barX = layout.x + (layout.width - barWidth) / 2;
  const int barY = layout.y + layout.height - 10;

  int fillWidth = barWidth * progress / 100;

  renderer.fillRect(barX, barY, fillWidth, barHeight, true);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BaseTheme::drawReadingProgressBar(const GfxRenderer& renderer, const size_t bookProgress) const {
  int vieweableMarginTop, vieweableMarginRight, vieweableMarginBottom, vieweableMarginLeft;
  renderer.getOrientedViewableTRBL(&vieweableMarginTop, &vieweableMarginRight, &vieweableMarginBottom,
                                   &vieweableMarginLeft);

  const int progressBarMaxWidth = renderer.getScreenWidth() - vieweableMarginLeft - vieweableMarginRight;
  const int progressBarY =
      renderer.getScreenHeight() - vieweableMarginBottom - BaseMetrics::values.bookProgressBarHeight;
  const int progressBarHeight = BaseMetrics::values.bookProgressBarHeight + vieweableMarginBottom;
  // At 100%, extend past the right viewable margin to the screen edge
  const int barWidth = (bookProgress >= 100) ? (renderer.getScreenWidth() - vieweableMarginLeft)
                                             : static_cast<int>(progressBarMaxWidth * bookProgress / 100);
  renderer.fillRect(vieweableMarginLeft, progressBarY, barWidth, progressBarHeight, true);
}

int BaseTheme::getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                       bool hasSubtitle) {
  const auto& metrics = BaseMetrics::values;
  int reservedHeight = metrics.topPadding;
  if (hasHeader) {
    reservedHeight += metrics.headerHeight + metrics.verticalSpacing;
  }
  if (hasTabBar) {
    reservedHeight += metrics.tabBarHeight;
  }
  if (hasButtonHints) {
    reservedHeight += metrics.verticalSpacing + metrics.buttonHintsHeight;
  }
  const int availableHeight = renderer.getScreenHeight() - reservedHeight;
  int rowHeight = hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  return availableHeight / rowHeight;
}

std::string BaseTheme::getCoverThumbPath(std::string coverBmpPath, int coverHeight) {
  size_t pos = coverBmpPath.find("[HEIGHT]", 0);
  if (pos != std::string::npos) {
    coverBmpPath.replace(pos, 8, std::to_string(coverHeight));
  }
  return coverBmpPath;
}
