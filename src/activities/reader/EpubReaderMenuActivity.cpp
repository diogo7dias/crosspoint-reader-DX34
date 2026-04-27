#include "EpubReaderMenuActivity.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ReadingThemeStore.h"
#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "util/FavoriteBmp.h"

EpubReaderMenuActivity::EpubReaderMenuActivity(
    GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title, const std::string& bookPath,
    const int currentPage, const int totalPages, const int bookProgressPercent, const uint8_t currentOrientation,
    const bool hasFootnotes, const bool isPageBookmarked, const int bookmarkCount, const bool hasQuotes,
    const std::function<void(uint8_t)>& onBack, const std::function<void(MenuAction)>& onAction)
    : ActivityWithSubactivity("EpubReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(hasFootnotes, isPageBookmarked, bookmarkCount, hasQuotes)),
      title(title),
      bookPath(bookPath),
      pendingOrientation(currentOrientation),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent),
      onBack(onBack),
      onAction(onAction) {}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildMenuItems(bool hasFootnotes,
                                                                                     bool isPageBookmarked,
                                                                                     int bookmarkCount,
                                                                                     bool hasQuotes) {
  std::vector<MenuItem> items;
  items.reserve(19);
  items.push_back({MenuAction::THEMES_MENU, StrId::STR_READING_THEMES});
  items.push_back({MenuAction::TOGGLE_BOLD_SWAP, StrId::STR_BOLD_SWAP});
  items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  items.push_back({MenuAction::HIGHLIGHT_QUOTE, StrId::STR_GRAB_QUOTE});
  if (hasQuotes) {
    items.push_back({MenuAction::VIEW_QUOTES, StrId::STR_VIEW_QUOTES});
  }
  items.push_back(
      {MenuAction::BOOKMARK_TOGGLE, isPageBookmarked ? StrId::STR_REMOVE_BOOKMARK : StrId::STR_ADD_BOOKMARK});
  if (bookmarkCount > 0) {
    items.push_back({MenuAction::BOOKMARK_LIST, StrId::STR_BOOKMARKS});
  }
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  // Removed: REVERT_THEME (use Reading Themes to manage), GO_HOME (Back button does this)
  items.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS});
  items.push_back({MenuAction::REMOVE_FROM_RECENT, StrId::STR_REMOVE_FROM_RECENTS});
  items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
  items.push_back({MenuAction::DELETE_BOOK, StrId::STR_DELETE_BOOK});

  // Wallpaper triage section (only if a last sleep wallpaper exists)
  const std::string& wallpaperPath = APP_STATE.lastSleepWallpaperPath;
  if (!wallpaperPath.empty() && Storage.exists(wallpaperPath.c_str())) {
    items.push_back({MenuAction::NONE, StrId::STR_WALLPAPER_TRIAGE, nullptr, true});
    const bool isFav = FavoriteBmp::isFavoritePath(wallpaperPath);
    items.push_back({MenuAction::TRIAGE_FAVORITE, isFav ? StrId::STR_UNFAVORITE : StrId::STR_FAVORITE});
    items.push_back({MenuAction::TRIAGE_PAUSE_ROTATION,
                     APP_STATE.wallpaperRotationPaused ? StrId::STR_TRIAGE_UNPAUSE : StrId::STR_TRIAGE_PAUSE});
    items.push_back({MenuAction::TRIAGE_MOVE_PAUSE, StrId::STR_MOVE_TO_SLEEP_PAUSE});
    items.push_back({MenuAction::TRIAGE_DELETE, StrId::STR_TRIAGE_DELETE});
  }

  // Extras section
  items.push_back({MenuAction::NONE, StrId::STR_EXTRAS, nullptr, true});
  items.push_back({MenuAction::TOGGLE_RANDOM_BOOK_ON_BOOT, StrId::STR_RANDOM_BOOK_ON_BOOT});
  items.push_back({MenuAction::SHARE_QR, StrId::STR_DOWNLOAD_BOOK_VIA_QR});

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
      next = (direction > 0) ? ButtonNavigator::nextIndex(next, count) : ButtonNavigator::previousIndex(next, count);
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
      // randomBookOnBoot is a true global (boot behavior), not a per-book
      // theme field, so it must persist to /.crosspoint/settings.json. But
      // the in-memory SETTINGS currently holds the per-book theme overlay
      // (applied at book open by ReadingThemeStore::loadBookSettingsIntoCurrent).
      // A naive SETTINGS.saveToFile() here would write that overlay to globals
      // and corrupt the user's defaults. Snapshot the per-book theme, reload
      // the on-disk globals, flip the flag, save, then re-apply the per-book
      // theme so the in-memory rendering state is preserved.
      const bool newValue = !SETTINGS.randomBookOnBoot;
      const ReadingTheme bookTheme = ReadingThemeStore::fromSettings("", SETTINGS);
      // Only persist if we successfully reloaded the on-disk globals — a
      // failed load would leave SETTINGS in a partially-initialised state and
      // saveToFile would then write that garbage back as the new global.
      if (SETTINGS.loadFromFile()) {
        SETTINGS.randomBookOnBoot = newValue;
        SETTINGS.saveToFile();
      }
      ReadingThemeStore::applyThemeToSettings(bookTheme, SETTINGS);
      SETTINGS.randomBookOnBoot = newValue;  // keep in-memory flag consistent for this session
      requestUpdate();
      return;
    }
    if (selectedAction == MenuAction::TOGGLE_BOLD_SWAP) {
      // Flip the per-book swap preference, persist it to RecentBooksStore,
      // and update the global EpdFontFamily state so any subsequent draws
      // (including this menu's own labels) reflect the new mapping. The
      // reader picks up the change on menu exit via
      // EpubReaderActivity::onReaderMenuBack, which re-lays out the current
      // page if the value changed while the menu was open.
      const bool newEnabled = !RECENT_BOOKS.getBoldSwap(bookPath);
      RECENT_BOOKS.setBoldSwap(bookPath, newEnabled);
      EpdFontFamily::setReaderBoldSwapEnabled(newEnabled);
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
  const auto pageHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const auto metrics = BaseMetrics::values;
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
  const int footerReserve = metrics.buttonHintsHeight + metrics.verticalSpacing;
  const int contentHeight = std::max(lineHeight, pageHeight - (startY + footerReserve));
  const int pageItems = std::max(1, contentHeight / lineHeight);
  const int pageStartIndex = (selectedIndex / pageItems) * pageItems;

  for (int i = pageStartIndex; i < static_cast<int>(menuItems.size()) && i < pageStartIndex + pageItems; ++i) {
    const int displayY = startY + ((i - pageStartIndex) * lineHeight);
    const auto& item = menuItems[i];

    if (item.isSeparator) {
      // Draw as an inverted header bar (same style as ReaderSettingsActivity)
      renderer.fillRect(contentX, displayY, contentWidth, lineHeight, true);
      const char* label = I18N.get(item.labelId);
      const int textW = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::REGULAR);
      renderer.drawText(UI_10_FONT_ID, contentX + (contentWidth - textW) / 2, displayY, label, false,
                        EpdFontFamily::REGULAR);
      continue;
    }

    const bool isSelected = (static_cast<int>(i) == selectedIndex);
    if (isSelected) {
      // Highlight only the content area so we don't paint over hint gutters.
      renderer.fillRect(contentX, displayY, contentWidth - 1, lineHeight, true);
    }

    const char* label = item.literalLabel != nullptr ? item.literalLabel : I18N.get(item.labelId);
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
    } else if (item.action == MenuAction::TOGGLE_BOLD_SWAP) {
      const char* value =
          RECENT_BOOKS.getBoldSwap(bookPath) ? I18N.get(StrId::STR_STATE_ON) : I18N.get(StrId::STR_STATE_OFF);
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, value);
      renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - 20 - width, displayY, value, !isSelected);
    }
  }

  // Footer / Hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (static_cast<int>(menuItems.size()) > pageItems) {
    const int currentPage = pageStartIndex / pageItems + 1;
    const int totalPages = (static_cast<int>(menuItems.size()) + pageItems - 1) / pageItems;
    const std::string pageCounter = std::to_string(currentPage) + "/" + std::to_string(totalPages);
    const int counterWidth = renderer.getTextWidth(UI_10_FONT_ID, pageCounter.c_str());
    const int counterY = pageHeight - metrics.buttonHintsHeight - renderer.getLineHeight(UI_10_FONT_ID) - 4;
    renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - 12 - counterWidth, counterY, pageCounter.c_str());
  }

  renderer.displayBuffer();
}
