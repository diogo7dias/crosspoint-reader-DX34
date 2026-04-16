#include "BookmarkListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "activities/util/ConfirmDialogActivity.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

namespace {
constexpr int kButtonHintsReserve = 50;
constexpr unsigned long kHoldDeleteMs = 2000;
}  // namespace

void BookmarkListActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  selectorIndex = 0;
  requestUpdate();
}

void BookmarkListActivity::onExit() { ActivityWithSubactivity::onExit(); }

std::string BookmarkListActivity::formatBookmark(
    const BookmarkStore::Bookmark& bm) const {
  std::string label;

  // Try to resolve chapter name from TOC
  if (epub) {
    const int tocIndex = epub->getTocIndexForSpineIndex(bm.spineIndex);
    if (tocIndex >= 0) {
      const auto tocItem = epub->getTocItem(tocIndex);
      label = tocItem.title;
      if (label.size() > 25) {
        label.resize(22);
        label += "...";
      }
      label += " p" + std::to_string(bm.pageNumber + 1);
    }
  }

  if (label.empty()) {
    label =
        "Ch " + std::to_string(bm.spineIndex) + " p" + std::to_string(bm.pageNumber + 1);
  }

  return label;
}

void BookmarkListActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  const int totalItems = store.count();

  if (totalItems == 0) {
    // Empty state — only back works
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoBack();
    }
    return;
  }

  // Hold Select: fire delete dialog as soon as threshold is reached (while held)
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= kHoldDeleteMs &&
      selectorIndex >= 0 && selectorIndex < totalItems) {
    mappedInput.suppressUntilAllReleased();
    const int deleteIdx = selectorIndex;
    const std::string label = formatBookmark(store.getAll()[deleteIdx]);
    const std::string msg = "Delete bookmark?\n" + label;
    enterNewActivity(new ConfirmDialogActivity(
        renderer, mappedInput, msg,
        [this, deleteIdx] {
          store.remove(deleteIdx);
          store.save(cachePath);
          if (selectorIndex >= store.count() && selectorIndex > 0)
            selectorIndex--;
          exitActivity();
          requestUpdate();
        },
        [this] {
          exitActivity();
          requestUpdate();
        }));
    return;
  }

  // Short press Select: jump to bookmark
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectorIndex >= 0 && selectorIndex < totalItems) {
      const auto& bm = store.getAll()[selectorIndex];
      const bool isCurrent =
          (bm.spineIndex == currentSpineIndex && bm.pageNumber == currentPage);
      if (isCurrent) {
        onGoBack();
      } else {
        const int spine = bm.spineIndex;
        const int page = bm.pageNumber;
        onJump(spine, page);
      }
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
    return;
  }

  buttonNavigator.onNext([this, totalItems] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, totalItems] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
    requestUpdate();
  });
}

void BookmarkListActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto screenHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw =
      orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw =
      orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted =
      orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;

  // Title
  const char* title = I18N.get(StrId::STR_BOOKMARKS);
  const int titleX =
      contentX +
      (contentWidth -
       renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::REGULAR)) /
          2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, title, true,
                    EpdFontFamily::REGULAR);

  const int totalItems = store.count();

  // Count indicator (e.g. "3 / 20")
  const std::string countStr = std::to_string(totalItems) + " / " +
                                std::to_string(BookmarkStore::MAX_BOOKMARKS);
  renderer.drawCenteredText(UI_10_FONT_ID, 42 + contentY, countStr.c_str());

  if (totalItems == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, screenHeight / 2,
                              I18N.get(StrId::STR_NO_BOOKMARKS));
    const auto labels =
        mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3,
                        labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int listStartY = 72 + contentY;
  const int availableHeight = screenHeight - listStartY - kButtonHintsReserve;
  const int itemsPerPage =
      std::max(1, availableHeight / kLineHeight);

  // Pagination
  const int pageStart = (selectorIndex / itemsPerPage) * itemsPerPage;

  int currentY = listStartY;
  for (int i = pageStart;
       i < totalItems && i < pageStart + itemsPerPage; i++) {
    const auto& bm = store.getAll()[i];
    const bool isSelected = (i == selectorIndex);
    const bool isCurrent =
        (bm.spineIndex == currentSpineIndex && bm.pageNumber == currentPage);

    if (isSelected) {
      renderer.fillRect(contentX, currentY - 2, contentWidth - 1, kLineHeight);
    }

    const std::string label = formatBookmark(bm);

    if (isCurrent && !isSelected) {
      // Current position: bold (double-draw) + dotted border
      renderer.drawText(UI_10_FONT_ID, contentX + 20, currentY,
                        label.c_str(), true);
      renderer.drawText(UI_10_FONT_ID, contentX + 21, currentY,
                        label.c_str(), true);
      const int rectX = contentX + 2;
      const int rectY = currentY - 2;
      const int rectW = contentWidth - 5;
      const int rectH = kLineHeight;
      for (int px = rectX; px < rectX + rectW; px += 3)  {
        renderer.drawPixel(px, rectY);
        renderer.drawPixel(px, rectY + rectH - 1);
      }
      for (int py = rectY; py < rectY + rectH; py += 3) {
        renderer.drawPixel(rectX, py);
        renderer.drawPixel(rectX + rectW - 1, py);
      }
    } else {
      renderer.drawText(UI_10_FONT_ID, contentX + 20, currentY,
                        label.c_str(), !isSelected);
    }

    currentY += kLineHeight;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Select\n/hold",
                                            tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3,
                      labels.btn4);

  renderer.displayBuffer();
}
