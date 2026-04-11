#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/FavoriteBmp.h"

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t currentOrientation,
                                               const bool hasFootnotes, const std::function<void(uint8_t)>& onBack,
                                               const std::function<void(MenuAction)>& onAction)
    : ActivityWithSubactivity("EpubReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(hasFootnotes)),
      title(title),
      pendingOrientation(currentOrientation),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent),
      onBack(onBack),
      onAction(onAction) {}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildMenuItems(bool hasFootnotes) {
  std::vector<MenuItem> items;
  items.reserve(15);
  items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  items.push_back({MenuAction::THEMES_MENU, StrId::STR_READING_THEMES});
  // Removed: REVERT_THEME (use Reading Themes to manage), GO_HOME (Back button does this)
  items.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS});
  items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
  items.push_back({MenuAction::DELETE_BOOK, StrId::STR_DELETE_BOOK});
  items.push_back({MenuAction::REMOVE_FROM_RECENT, StrId::STR_REMOVE_FROM_RECENTS});

  // Wallpaper triage section (only if a last sleep wallpaper exists)
  const std::string& wallpaperPath = APP_STATE.lastSleepWallpaperPath;
  if (!wallpaperPath.empty() && Storage.exists(wallpaperPath.c_str())) {
    items.push_back({MenuAction::NONE, StrId::STR_WALLPAPER_TRIAGE, nullptr, true});
    const bool isFav = FavoriteBmp::isFavoritePath(wallpaperPath);
    items.push_back({MenuAction::TRIAGE_FAVORITE,
                     isFav ? StrId::STR_UNFAVORITE : StrId::STR_FAVORITE});
    items.push_back({MenuAction::TRIAGE_PAUSE_ROTATION,
                     APP_STATE.wallpaperRotationPaused ? StrId::STR_TRIAGE_UNPAUSE
                                                      : StrId::STR_TRIAGE_PAUSE});
    items.push_back({MenuAction::TRIAGE_MOVE_PAUSE, StrId::STR_MOVE_TO_SLEEP_PAUSE});
    items.push_back({MenuAction::TRIAGE_DELETE, StrId::STR_TRIAGE_DELETE});
  }

  items.push_back({MenuAction::TOGGLE_RANDOM_BOOK_ON_BOOT, StrId::STR_RANDOM_BOOK_ON_BOOT});

  return items;
}

void EpubReaderMenuActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  requestUpdate();
}

void EpubReaderMenuActivity::onExit() { ActivityWithSubactivity::onExit(); }

void EpubReaderMenuActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  // Handle navigation (skip separator rows)
  const auto advanceIndex = [this](int direction) {
    const int count = static_cast<int>(menuItems.size());
    int next = selectedIndex;
    for (int i = 0; i < count; i++) {
      next = (direction > 0) ? ButtonNavigator::nextIndex(next, count)
                             : ButtonNavigator::previousIndex(next, count);
      if (!menuItems[next].isSeparator) {
        selectedIndex = next;
        break;
      }
    }
    requestUpdate();
  };

  buttonNavigator.onNext([this, &advanceIndex] { advanceIndex(+1); });
  buttonNavigator.onPrevious([this, &advanceIndex] { advanceIndex(-1); });

  // Use local variables for items we need to check after potential deletion
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto selectedAction = menuItems[selectedIndex].action;
    if (selectedAction == MenuAction::ROTATE_SCREEN) {
      // Cycle orientation preview locally; actual rotation happens on menu exit.
      pendingOrientation = (pendingOrientation + 1) % orientationLabels.size();
      requestUpdate();
      return;
    }
    if (selectedAction == MenuAction::TOGGLE_RANDOM_BOOK_ON_BOOT) {
      SETTINGS.randomBookOnBoot = !SETTINGS.randomBookOnBoot;
      SETTINGS.saveToFile();
      requestUpdate();
      return;
    }

    // 1. Capture the callback and action locally
    auto actionCallback = onAction;

    // 2. Execute the callback
    actionCallback(selectedAction);

    // 3. CRITICAL: Return immediately. 'this' is likely deleted now.
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Return the pending orientation to the parent so it can apply on exit.
    onBack(pendingOrientation);
    return;  // Also return here just in case
  }
}

void EpubReaderMenuActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  // Landscape orientation: button hints are drawn along a vertical edge, so we
  // reserve a horizontal gutter to prevent overlap with menu content.
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  // Inverted portrait: button hints appear near the logical top, so we reserve
  // vertical space to keep the header and list clear.
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  // Landscape CW places hints on the left edge; CCW keeps them on the right.
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;

  // Title
  const std::string truncTitle =
      renderer.truncatedText(UI_12_FONT_ID, title.c_str(), contentWidth - 40, EpdFontFamily::REGULAR);
  // Manual centering so we can respect the content gutter.
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, truncTitle.c_str(), EpdFontFamily::REGULAR)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, truncTitle.c_str(), true, EpdFontFamily::REGULAR);

  // Progress summary
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  renderer.drawCenteredText(UI_10_FONT_ID, 45, progressLine.c_str());

  // Menu Items
  const int startY = 75 + contentY;
  constexpr int lineHeight = 30;

  for (size_t i = 0; i < menuItems.size(); ++i) {
    const int displayY = startY + (i * lineHeight);
    const auto& item = menuItems[i];

    if (item.isSeparator) {
      // Draw as an inverted header bar (same style as ReaderSettingsActivity)
      renderer.fillRect(contentX, displayY, contentWidth, lineHeight, true);
      const char* label = I18N.get(item.labelId);
      const int textW = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::REGULAR);
      renderer.drawText(UI_10_FONT_ID, contentX + (contentWidth - textW) / 2,
                        displayY, label, false, EpdFontFamily::REGULAR);
      continue;
    }

    const bool isSelected = (static_cast<int>(i) == selectedIndex);
    if (isSelected) {
      // Highlight only the content area so we don't paint over hint gutters.
      renderer.fillRect(contentX, displayY, contentWidth - 1, lineHeight, true);
    }

    const char* label = item.literalLabel != nullptr
                            ? item.literalLabel
                            : I18N.get(item.labelId);
    renderer.drawText(UI_10_FONT_ID, contentX + 20, displayY, label, !isSelected);

    if (item.action == MenuAction::ROTATE_SCREEN) {
      // Render current orientation value on the right edge of the content area.
      const char* value = I18N.get(orientationLabels[pendingOrientation]);
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, value);
      renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - 20 - width, displayY, value, !isSelected);
    } else if (item.action == MenuAction::TOGGLE_RANDOM_BOOK_ON_BOOT) {
      const char* value = SETTINGS.randomBookOnBoot ? I18N.get(StrId::STR_STATE_ON) : I18N.get(StrId::STR_STATE_OFF);
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, value);
      renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - 20 - width, displayY, value, !isSelected);
    }
  }

  // Footer / Hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
