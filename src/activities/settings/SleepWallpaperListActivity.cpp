#include "SleepWallpaperListActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "sleep/SleepFs.h"
#include "sleep/WallpaperPlaylistV2.h"

namespace {
// Space reserved for the bottom button hints (portrait) so rows don't overlap.
constexpr int kButtonHintsReserve = 50;
}  // namespace

crosspoint::sleep::OrderPage SleepWallpaperListActivity::loadPage(size_t start, size_t count,
                                                                  bool& fromOrderFile) const {
  using crosspoint::sleep::OrderPage;
  fromOrderFile = false;

  const auto& deps = crosspoint::sleep::v2::WallpaperPlaylistV2::instance().deps();
  const std::string& orderPath = deps.orderFilePath;

  // Preferred path: stream the order file (true rotation order). Read in fixed
  // chunks and split on '\n' so we never hold more than one line plus the page
  // window — the whole point of this screen vs. a safeRead of the file.
  if (Storage.exists(orderPath.c_str())) {
    HalFile f;
    if (Storage.openFileForRead("SWL", orderPath, f)) {
      char buf[256] = {0};
      int len = 0;
      int pos = 0;
      auto reader = [&](std::string& out) -> bool {
        out.clear();
        bool gotAny = false;
        for (;;) {
          if (pos >= len) {
            len = f.read(buf, sizeof(buf));
            pos = 0;
            if (len <= 0) return gotAny;  // EOF: emit a trailing line lacking '\n', else stop
          }
          const char c = buf[pos++];
          if (c == '\n') return true;
          if (c == '\r') continue;
          out.push_back(c);
          gotAny = true;
        }
      };
      OrderPage page = crosspoint::sleep::readSleepOrderPage(reader, start, count);
      f.close();
      if (page.total > 0) {
        fromOrderFile = true;
        return page;
      }
    }
  }

  // Fallback: no order file yet (fresh card / never reconciled). Walk /sleep
  // directly in SD order — also O(1) heap, collecting only the page window.
  OrderPage page;
  crosspoint::sleep::ISleepFs* fs = deps.fs;
  if (fs) {
    size_t idx = 0;
    fs->walkSleepBmps([&](const char* name, size_t nlen, uint32_t /*mtime*/) {
      const size_t i = idx++;
      if (i >= start && i < start + count) page.names.emplace_back(name, nlen);
    });
    page.total = idx;
  }
  return page;
}

void SleepWallpaperListActivity::onEnter() {
  Activity::onEnter();
  selectorIndex = 0;
  // A count=0 pass scans for the total without materializing any names.
  bool ordered = false;
  const crosspoint::sleep::OrderPage probe = loadPage(0, 0, ordered);
  totalCount = probe.total;
  orderedSource = ordered;
  requestUpdate();
}

void SleepWallpaperListActivity::onExit() { Activity::onExit(); }

void SleepWallpaperListActivity::loop() {
  const int total = static_cast<int>(totalCount);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (total <= 0) return;

  buttonNavigator.onNextRelease([this, total] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, total);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, total] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, total);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this, total] {
    for (int i = 0; i < 5 && selectorIndex < total - 1; i++) {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, total);
    }
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, total] {
    for (int i = 0; i < 5 && selectorIndex > 0; i++) {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, total);
    }
    requestUpdate();
  });
}

void SleepWallpaperListActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;

  // Title.
  const char* title = tr(STR_VIEW_SLEEP_WALLPAPERS);
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::REGULAR)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, title, true, EpdFontFamily::REGULAR);

  // When no order file exists yet, the list reflects raw folder (filename)
  // order rather than the true rotation order — say so under the title.
  if (totalCount > 0 && !orderedSource) {
    const char* note = tr(STR_WALLPAPER_LIST_FILENAME_ORDER);
    const int noteX = contentX + (contentWidth - renderer.getTextWidth(UI_10_FONT_ID, note)) / 2;
    renderer.drawText(UI_10_FONT_ID, noteX, 38 + contentY, note, true);
  }

  const int listStartY = 60 + contentY;
  const int availableHeight = screenHeight - listStartY - kButtonHintsReserve;
  const int itemsPerPage = (availableHeight > 0) ? std::max(1, availableHeight / kLineHeight) : 1;

  if (totalCount == 0) {
    const char* empty = tr(STR_NO_SLEEP_WALLPAPERS);
    const int emptyX = contentX + (contentWidth - renderer.getTextWidth(UI_10_FONT_ID, empty)) / 2;
    renderer.drawText(UI_10_FONT_ID, emptyX, listStartY + 20, empty, true);
    const auto emptyLabels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_BACK), "", "");
    GUI.drawButtonHints(renderer, emptyLabels.btn1, emptyLabels.btn2, emptyLabels.btn3, emptyLabels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int pageStart = (selectorIndex / itemsPerPage) * itemsPerPage;
  bool ordered = false;
  const crosspoint::sleep::OrderPage page =
      loadPage(static_cast<size_t>(pageStart), static_cast<size_t>(itemsPerPage), ordered);
  orderedSource = ordered;

  const std::string& lastShown = APP_STATE.lastShownSleepFilename;
  const int markerReserve = 16;  // left gutter for the "currently shown" dot
  const int textX = contentX + 20 + markerReserve;
  const int textWidth = contentWidth - 40 - markerReserve;

  int currentY = listStartY;
  for (size_t row = 0; row < page.names.size(); ++row) {
    const int itemIndex = pageStart + static_cast<int>(row);
    const std::string& name = page.names[row];
    const bool isSelected = (itemIndex == selectorIndex);
    const bool isCurrent = (!lastShown.empty() && name == lastShown);

    if (isSelected) {
      renderer.fillRect(contentX, currentY - 2, contentWidth - 1, kLineHeight);
    }

    // "Currently shown" marker: a filled dot in the left gutter.
    if (isCurrent) {
      const int dotX = contentX + 10;
      const int dotY = currentY + kLineHeight / 2 - 4;
      renderer.fillRect(dotX, dotY, 6, 6, !isSelected);
    }

    const std::string shown = renderer.truncatedText(UI_10_FONT_ID, name.c_str(), std::max(1, textWidth));
    renderer.drawText(UI_10_FONT_ID, textX, currentY, shown.c_str(), !isSelected);

    currentY += kLineHeight;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_BACK), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
