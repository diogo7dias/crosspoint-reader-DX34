#include "EpubReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "ReaderLayoutSafety.h"
#include "activities/util/ConfirmDialogActivity.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

namespace {
// Space reserved for the bottom button hints (portrait) so items don't overlap.
constexpr int kButtonHintsReserve = 50;
}  // namespace

int EpubReaderChapterSelectionActivity::getTotalItems() const { return epub->getTocItemsCount(); }

bool EpubReaderChapterSelectionActivity::rowShowsPageCount(int tocIndex) const {
  // Lector: only the chapter currently being read shows a page count (its exact,
  // already-paginated length). The old byte-ratio estimate for other chapters was
  // too unreliable on image/code-heavy chapters, so it was removed.
  return currentSectionPageCount > 0 && tocIndex == resolvedCurrentTocIndex;
}

int EpubReaderChapterSelectionActivity::estimateChapterPages(int tocIndex) const {
  if (!epub) return 0;
  const int spineIndex = epub->getTocItem(tocIndex).spineIndex;
  if (spineIndex < 0) return 0;

  // Only the chapter currently being read has an exact, already-paginated count.
  // Every other chapter returns 0 (no count shown).
  if (spineIndex == currentSpineIndex && currentSectionPageCount > 0) {
    return currentSectionPageCount;
  }
  return 0;
}

int EpubReaderChapterSelectionActivity::getItemLineCount(int itemIndex, int maxTextWidth) const {
  const auto item = epub->getTocItem(itemIndex);
  const int indentSize = 20 + (item.level - 1) * 15;
  const int countReserve = rowShowsPageCount(itemIndex) ? kPageCountReserve : 0;
  const int textWidth = maxTextWidth - 40 - indentSize - countReserve;
  if (textWidth <= 0) return 1;
  const auto lines = ReaderLayoutSafety::wrapText(renderer, UI_10_FONT_ID, item.title, textWidth);
  return std::max(1, static_cast<int>(lines.size()));
}

int EpubReaderChapterSelectionActivity::computePageStart(int availableHeight, int maxTextWidth) const {
  const int totalItems = getTotalItems();
  if (totalItems == 0) return 0;

  // Walk forward from item 0, filling pages until we find the one containing selectorIndex.
  int pageStart = 0;
  while (pageStart < totalItems) {
    int usedHeight = 0;
    int idx = pageStart;
    while (idx < totalItems) {
      const int itemH = getItemLineCount(idx, maxTextWidth) * kLineHeight;
      if (usedHeight + itemH > availableHeight && idx > pageStart) break;
      usedHeight += itemH;
      idx++;
    }
    // Page covers [pageStart, idx)
    if (selectorIndex < idx) return pageStart;
    pageStart = idx;
  }
  return pageStart;
}

void EpubReaderChapterSelectionActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (!epub) {
    return;
  }

  selectorIndex = currentTocIndex;
  if (selectorIndex < 0) {
    selectorIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  }
  if (selectorIndex == -1) {
    selectorIndex = 0;
  }
  resolvedCurrentTocIndex = selectorIndex;

  lastNavReleaseMs = 0;
  lastNavDirection = 0;

  // Trigger first update
  requestUpdate();
}

void EpubReaderChapterSelectionActivity::onExit() { ActivityWithSubactivity::onExit(); }

void EpubReaderChapterSelectionActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  const int totalItems = getTotalItems();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectorIndex < 0 || selectorIndex >= totalItems) {
      onGoBack();
    } else if (selectorIndex == resolvedCurrentTocIndex) {
      // Already on this chapter — just go back
      onGoBack();
    } else {
      // Different chapter — show confirmation
      const auto tocItem = epub->getTocItem(selectorIndex);
      const std::string msg = std::string(tr(STR_JUMP_TO_CHAPTER)) + "?\n" + tocItem.title;
      const int selectedIdx = selectorIndex;
      enterNewActivity(new (std::nothrow) ConfirmDialogActivity(
          renderer, mappedInput, msg, [this, selectedIdx] { onSelectTocIndex(selectedIdx); },
          [this] {
            exitActivity();
            requestUpdate();
          }));
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
  }

  // Up/Down: single tap navigates, double-tap jumps to current chapter.
  // First tap is instant (no delay); second tap within window triggers jump.
  buttonNavigator.onNextRelease([this, totalItems] {
    const unsigned long now = millis();
    if (lastNavDirection == 1 && now - lastNavReleaseMs <= kDoubleTapMs) {
      // Double-tap Down: jump to current chapter
      selectorIndex = resolvedCurrentTocIndex;
      lastNavDirection = 0;
    } else {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
      lastNavDirection = 1;
      lastNavReleaseMs = now;
    }
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, totalItems] {
    const unsigned long now = millis();
    if (lastNavDirection == -1 && now - lastNavReleaseMs <= kDoubleTapMs) {
      // Double-tap Up: jump to current chapter
      selectorIndex = resolvedCurrentTocIndex;
      lastNavDirection = 0;
    } else {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
      lastNavDirection = -1;
      lastNavReleaseMs = now;
    }
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, totalItems] {
    lastNavDirection = 0;  // Cancel double-tap tracking on hold
    for (int i = 0; i < 5 && selectorIndex < totalItems - 1; i++) {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
    }
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, totalItems] {
    lastNavDirection = 0;
    for (int i = 0; i < 5 && selectorIndex > 0; i++) {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
    }
    requestUpdate();
  });
}

void EpubReaderChapterSelectionActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto screenHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;
  const int totalItems = getTotalItems();

  // Title
  const int titleX =
      contentX +
      (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_SELECT_CHAPTER), EpdFontFamily::REGULAR)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_SELECT_CHAPTER), true, EpdFontFamily::REGULAR);

  const int listStartY = 60 + contentY;
  // Reserve space for button hints at the bottom of the screen.
  const int availableHeight = screenHeight - listStartY - kButtonHintsReserve;
  const int maxTextWidth = contentWidth;

  // Compute which page of items to display
  const int pageStartIndex = computePageStart(availableHeight, maxTextWidth);

  // Render items with variable height
  int currentY = listStartY;
  for (int itemIndex = pageStartIndex; itemIndex < totalItems; itemIndex++) {
    const auto item = epub->getTocItem(itemIndex);
    const int indentSize = contentX + 20 + (item.level - 1) * 15;
    const int countReserve = rowShowsPageCount(itemIndex) ? kPageCountReserve : 0;
    const int textWidth = maxTextWidth - 40 - indentSize - countReserve;

    std::vector<std::string> lines;
    if (textWidth > 0) {
      lines = ReaderLayoutSafety::wrapText(renderer, UI_10_FONT_ID, item.title, textWidth);
    }
    if (lines.empty()) {
      lines.push_back(renderer.truncatedText(UI_10_FONT_ID, item.title.c_str(), std::max(1, textWidth)));
    }

    const int itemHeight = static_cast<int>(lines.size()) * kLineHeight;

    // Stop if this item would overflow the available area
    if (currentY + itemHeight > listStartY + availableHeight && itemIndex > pageStartIndex) break;

    const bool isSelected = (itemIndex == selectorIndex);
    const bool isCurrent = (itemIndex == resolvedCurrentTocIndex);

    // Selection highlight
    if (isSelected) {
      renderer.fillRect(contentX, currentY - 2, contentWidth - 1, itemHeight);
    }

    if (isCurrent && !isSelected) {
      // Current reading position: bold text (double-draw) + dotted border
      for (int l = 0; l < static_cast<int>(lines.size()); l++) {
        renderer.drawText(UI_10_FONT_ID, indentSize, currentY + l * kLineHeight, lines[l].c_str(), true);
        renderer.drawText(UI_10_FONT_ID, indentSize + 1, currentY + l * kLineHeight, lines[l].c_str(), true);
      }
      // Dotted border around item row
      const int rectX = contentX + 2;
      const int rectY = currentY - 2;
      const int rectW = contentWidth - 5;
      const int rectH = itemHeight;
      for (int px = rectX; px < rectX + rectW; px += 3) {
        renderer.drawPixel(px, rectY);
        renderer.drawPixel(px, rectY + rectH - 1);
      }
      for (int py = rectY; py < rectY + rectH; py += 3) {
        renderer.drawPixel(rectX, py);
        renderer.drawPixel(rectX + rectW - 1, py);
      }
    } else {
      for (int l = 0; l < static_cast<int>(lines.size()); l++) {
        renderer.drawText(UI_10_FONT_ID, indentSize, currentY + l * kLineHeight, lines[l].c_str(), !isSelected);
      }
    }

    // Right-aligned chapter length badge (estimated pages at the active font/settings).
    if (countReserve > 0) {
      const int pages = estimateChapterPages(itemIndex);
      if (pages > 0) {
        const std::string badge = std::to_string(pages) + "p";
        const int badgeWidth = renderer.getTextWidth(UI_10_FONT_ID, badge.c_str());
        const int badgeX = contentX + contentWidth - 6 - badgeWidth;
        // Match the title: dark text normally, light when the row is selected (inverted),
        // and bold (double-draw) for the current chapter to mirror its emphasized title.
        renderer.drawText(UI_10_FONT_ID, badgeX, currentY, badge.c_str(), !isSelected);
        if (isCurrent && !isSelected) {
          renderer.drawText(UI_10_FONT_ID, badgeX + 1, currentY, badge.c_str(), true);
        }
      }
    }

    currentY += itemHeight;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
