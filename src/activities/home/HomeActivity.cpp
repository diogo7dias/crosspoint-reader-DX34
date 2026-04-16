#include "HomeActivity.h"
#include "activities/boot_sleep/SleepActivity.h"
#include "activities/util/ConfirmDialogActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "Battery.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "util/BookProgress.h"
#include "util/DrawUtils.h"
#include "util/FavoriteBmp.h"
#include "util/StringUtils.h"

namespace {
constexpr const char *seeMoreLabel = "See all...";

enum class HomeMenuAction : uint8_t {
  BrowseFiles,
  OpdsBrowser,
  FileTransfer,
  Settings,
};

struct HomeMenuItem {
  const char *label;
  HomeMenuAction action;
  bool emphasized;
};

std::string getHomeHeaderVersionLabel() {
  const std::string rawVersion = CROSSPOINT_VERSION;
  const size_t dashPos = rawVersion.find_last_of('-');
  const std::string semver =
      (dashPos != std::string::npos && dashPos + 1 < rawVersion.size())
          ? rawVersion.substr(dashPos + 1)
          : rawVersion;
  return "DX34 [" + semver + "]";
}

std::vector<HomeMenuItem> buildHomeMenuItems(bool hasOpdsUrl) {
  std::vector<HomeMenuItem> items;
  items.reserve(hasOpdsUrl ? 4 : 3);
  items.push_back(
      {tr(STR_BROWSE_FILES), HomeMenuAction::BrowseFiles, false});
  if (hasOpdsUrl) {
    items.push_back(
        {tr(STR_OPDS_BROWSER), HomeMenuAction::OpdsBrowser, false});
  }
  items.push_back(
      {tr(STR_FILE_TRANSFER), HomeMenuAction::FileTransfer, false});
  items.push_back({tr(STR_SETTINGS_TITLE), HomeMenuAction::Settings, false});
  return items;
}

// Use shared DrawUtils::drawDottedRect instead of local copy

} // namespace

int HomeActivity::getMenuItemCount() const {
  return getRecentSlotCount() + 1 +
         static_cast<int>(buildHomeMenuItems(hasOpdsUrl).size());
}

int HomeActivity::getRecentSlotCount() const {
  return std::max(1, static_cast<int>(recentBooks.size()));
}

void HomeActivity::refreshSleepFavoriteWarning() {
  // Use the count cached by trimSleepFolderToLimit() (called in onGoHome)
  // instead of re-scanning the /sleep directory.
  protectedSleepFavoriteCount = SleepActivity::cachedSleepFavoriteCount();
  sleepFavoritesFull =
      protectedSleepFavoriteCount >= CrossPointState::SLEEP_PLAYLIST_MAX_PERSIST;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  if (maxBooks <= 0) {
    return;
  }

  const auto &books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(maxBooks);
  size_t eligibleCount = 0;
  const size_t maxVisibleBooks = static_cast<size_t>(maxBooks);
  const bool isClassic = SETTINGS.homeLayout == CrossPointSettings::HOME_LAYOUT_CLASSIC;
  std::unordered_set<std::string> seenPaths;
  seenPaths.reserve(books.size());

  for (const RecentBook &book : books) {
    if (!seenPaths.insert(book.path).second) {
      continue;
    }

    // Hide QUOTES sidecar files from the recents list
    if (book.path.size() >= 11 &&
        book.path.compare(book.path.size() - 11, 11, "_QUOTES.txt") == 0) {
      continue;
    }

    // Once we have enough visible books, we only need to confirm one more
    // eligible entry to show "See all...", then we can stop iterating.
    if (recentBooks.size() >= maxVisibleBooks) {
      if (Storage.exists(book.path.c_str())) {
        eligibleCount++;
        break;  // Found one more — enough to show "See all..."
      }
      continue;
    }

    // Skip if file no longer exists
    if (!Storage.exists(book.path.c_str())) {
      continue;
    }

    eligibleCount++;
    RecentBook entry = book;
    if (isClassic) {
      const auto percent = BookProgress::getPercent(book.path);
      entry.title =
          "[" + (percent.has_value() ? std::to_string(percent.value()) : "0") +
          "%]  " + book.title;
      // Classic mode should never attempt to load/render cover images.
      entry.coverBmpPath.clear();
    }
    recentBooks.push_back(entry);
  }

  if (eligibleCount > maxVisibleBooks && !recentBooks.empty()) {
    // Keep one slot for "See all..." when there are hidden ongoing books.
    if (recentBooks.size() == maxVisibleBooks) {
      recentBooks.pop_back();
    }
    RecentBook seeMoreRow;
    seeMoreRow.path.clear();
    seeMoreRow.title = seeMoreLabel;
    seeMoreRow.author.clear();
    seeMoreRow.coverBmpPath.clear();
    recentBooks.push_back(std::move(seeMoreRow));
  }
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  // Sleep folder trimming is handled by onGoHome() before this activity is
  // created, so we only need to read the cached favorite count here.
  refreshSleepFavoriteWarning();

  // Check if OPDS browser URL is configured
  hasOpdsUrl = strlen(SETTINGS.opdsServerUrl) > 0;

  selectorIndex = 1;  // Default focus on first recent book
  scrollOffset = 0;

  auto metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);
  // Half refresh on first render to clear ghosting from previous activity
  renderer.requestHalfRefresh();
  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();
}

void HomeActivity::loop() {
  // Let sub-activity (e.g. confirm dialog) handle input when active
  if (subActivity) {
    ActivityWithSubactivity::loop();
    return;
  }

  const int recentSlots = getRecentSlotCount();
  const auto menuItems = buildHomeMenuItems(hasOpdsUrl);
  const int menuCount = getMenuItemCount();

  // For single cover layout, visibility is always 1 book
  if (SETTINGS.homeLayout == CrossPointSettings::HOME_LAYOUT_SINGLE_COVER) {
    firstVisibleBookIdx = scrollOffset;
    lastVisibleBookIdx = scrollOffset;
  }

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    const int bookCount = static_cast<int>(recentBooks.size());
    if (selectorIndex >= 1 && selectorIndex <= bookCount) {
      const int bookIdx = selectorIndex - 1;
      if (bookIdx > lastVisibleBookIdx) {
        scrollOffset++;
      }
      if (bookIdx < firstVisibleBookIdx) {
        scrollOffset = bookIdx;
      }
      scrollOffset = std::max(0, std::min(scrollOffset, bookCount - 1));
    } else if (selectorIndex == 0) {
      scrollOffset = 0;
    }
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    const int bookCount = static_cast<int>(recentBooks.size());
    if (selectorIndex >= 1 && selectorIndex <= bookCount) {
      const int bookIdx = selectorIndex - 1;
      if (bookIdx < firstVisibleBookIdx) {
        scrollOffset = bookIdx;
      }
      scrollOffset = std::max(0, std::min(scrollOffset, bookCount - 1));
    } else if (selectorIndex == 0) {
      scrollOffset = 0;
    }
    requestUpdate();
  });

  // Back button: remove selected recent book from recents list
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (selectorIndex >= 1 && selectorIndex <= static_cast<int>(recentBooks.size())) {
      const int bookIndex = selectorIndex - 1;
      const std::string &selectedPath = recentBooks[bookIndex].path;
      // Only for actual books, not "See all..."
      if (!selectedPath.empty()) {
        const std::string title = recentBooks[bookIndex].title;
        const std::string path = selectedPath;
        enterNewActivity(new ConfirmDialogActivity(
            renderer, mappedInput,
            std::string(tr(STR_REMOVE_FROM_RECENTS)) + "?\n" + title,
            [this, path]() {
              RECENT_BOOKS.removeBook(path);
              auto metrics = UITheme::getInstance().getMetrics();
              loadRecentBooks(metrics.homeRecentBooksCount);
              // Reset selector if it's now out of bounds
              const int menuCount = getMenuItemCount();
              if (selectorIndex >= menuCount) {
                selectorIndex = std::max(0, menuCount - 1);
              }
              // Clamp scroll offset after removal
              const int maxOffset = std::max(0, static_cast<int>(recentBooks.size()) - 1);
              if (scrollOffset > maxOffset) {
                scrollOffset = maxOffset;
              }
              // exitActivity destroys the ConfirmDialog (and this lambda's
              // captured data), so it must come after all captured vars are used.
              exitActivity();
              requestUpdate();
            },
            [this]() {
              exitActivity();
              requestUpdate();
            }));
        return;
      }
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectorIndex == 0) {
      // Pages read counter (tap to reset)
      APP_STATE.sessionPagesRead = 0;
      APP_STATE.saveToFile();
      requestUpdate();
    } else if (selectorIndex <= static_cast<int>(recentBooks.size())) {
      const int bookIndex = selectorIndex - 1;
      const std::string &selectedPath = recentBooks[bookIndex].path;
      if (selectedPath.empty()) {
        onRecentsOpen();
      } else {
        onSelectBook(selectedPath);
      }
    } else if (selectorIndex < recentSlots + 1) {
      onMyLibraryOpen();
    } else {
      const int menuSelectedIndex = selectorIndex - recentSlots - 1;
      if (menuSelectedIndex >= static_cast<int>(menuItems.size())) {
        return;
      }

      switch (menuItems[menuSelectedIndex].action) {
      case HomeMenuAction::BrowseFiles:
        onMyLibraryOpen();
        break;
      case HomeMenuAction::OpdsBrowser:
        onOpdsBrowserOpen();
        break;
      case HomeMenuAction::FileTransfer:
        onFileTransferOpen();
        break;
      case HomeMenuAction::Settings:
        onSettingsOpen();
        break;
      }
    }
  }
}

void HomeActivity::render(Activity::RenderLock &&) {
  auto metrics = UITheme::getInstance().getMetrics();
  const int recentSlots = getRecentSlotCount();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer,
                 Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                 nullptr);
  const std::string homeVersionLabel = getHomeHeaderVersionLabel();
  const int versionY = metrics.topPadding + 5;
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, versionY,
                    homeVersionLabel.c_str());

  int warningBottomY =
      versionY + renderer.getLineHeight(UI_10_FONT_ID) + 12;
  if (sleepFavoritesFull) {
    const int warningY = warningBottomY;
    const int warningWidth = pageWidth - metrics.contentSidePadding * 2;
    const std::string warningText = renderer.truncatedText(
        SMALL_FONT_ID, FavoriteBmp::limitReachedHomeMessage(), warningWidth);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, warningY,
                      warningText.c_str());
    warningBottomY = warningY + renderer.getLineHeight(SMALL_FONT_ID) + 8;
  }

  const int sessionStatFont = UI_10_FONT_ID;
  const int sessionStatY = warningBottomY;
  const std::string labelText = tr(STR_SESSION_PAGES);
  const std::string countText = std::to_string(APP_STATE.sessionPagesRead);
  const bool pagesSelected = selectorIndex == 0;
  const int labelTextW = renderer.getTextWidth(sessionStatFont, labelText.c_str());
  const int countTextW = renderer.getTextWidth(sessionStatFont, countText.c_str());
  const int tilePad = 8;
  const int tileGap = 6;
  const int tileH = renderer.getLineHeight(sessionStatFont) + 6;

  // Label tile: "Pages Read" with border (or filled when selected)
  const int labelTileX = metrics.contentSidePadding;
  const int labelTileW = labelTextW + tilePad * 2;
  const int labelTileY = sessionStatY - 3;
  if (pagesSelected) {
    renderer.fillRect(labelTileX, labelTileY, labelTileW, tileH);
  } else {
    renderer.drawRect(labelTileX, labelTileY, labelTileW, tileH, 2, true);
  }
  renderer.drawText(sessionStatFont, labelTileX + tilePad,
                    labelTileY + 3, labelText.c_str(), !pagesSelected);

  // Count tile: black background with white text (like "N more below" indicator)
  const int countTileX = labelTileX + labelTileW + tileGap;
  const int countTileW = countTextW + tilePad * 2 + 8;
  renderer.fillRect(countTileX, labelTileY, countTileW, tileH);
  const int countTextX = countTileX + (countTileW - countTextW) / 2;
  renderer.drawText(sessionStatFont, countTextX,
                    labelTileY + 3, countText.c_str(), false);
  warningBottomY =
      labelTileY + tileH + 9;

  const auto menuItems = buildHomeMenuItems(hasOpdsUrl);
  const int menuCount = static_cast<int>(menuItems.size());
  const int menuBlockHeight =
      metrics.verticalSpacing + menuCount * metrics.menuRowHeight +
      (menuCount > 0 ? (menuCount - 1) * metrics.menuSpacing : 0);
  const int menuBottomGap = 8; // Keep a small gap above bottom button hints.
  const int menuY =
      pageHeight - metrics.buttonHintsHeight - menuBottomGap - menuBlockHeight;

  const int recentAreaBottomGap = 8;
  const int recentAreaY = warningBottomY;
  const int recentAreaHeight =
      std::max(0, menuY - recentAreaBottomGap - recentAreaY);
  switch (SETTINGS.homeLayout) {
    case CrossPointSettings::HOME_LAYOUT_SINGLE_COVER: {
      GUI.drawRecentBookSingleCover(
          renderer, Rect{0, recentAreaY, pageWidth, recentAreaHeight}, recentBooks,
          selectorIndex - 1, scrollOffset);
      break;
    }
    default: {
      auto vis = GUI.drawRecentBookCover(
          renderer, Rect{0, recentAreaY, pageWidth, recentAreaHeight}, recentBooks,
          selectorIndex - 1, scrollOffset);
      firstVisibleBookIdx = vis.firstVisible;
      lastVisibleBookIdx = vis.lastVisible;
      // Sync scrollOffset with renderer's adjusted position
      // (renderer may have adjusted it to keep the selected book visible)
      scrollOffset = vis.firstVisible;
      break;
    }
  }

  for (int i = 0; i < menuCount; ++i) {
    const int tileY =
        metrics.verticalSpacing + menuY +
        i * (metrics.menuRowHeight + metrics.menuSpacing);
    const int tileX = metrics.contentSidePadding;
    const int tileWidth = pageWidth - metrics.contentSidePadding * 2;
    const bool selected = selectorIndex - recentSlots - 1 == i;
    const bool emphasized = menuItems[i].emphasized;
    const auto textStyle =
        (emphasized && selected) ? EpdFontFamily::BOLD
                                 : EpdFontFamily::REGULAR;

    if (emphasized) {
      if (selected) {
        renderer.drawRect(tileX, tileY, tileWidth, metrics.menuRowHeight, 2,
                          true);
      } else {
        renderer.fillRect(tileX, tileY, tileWidth, metrics.menuRowHeight);
      }
    } else if (selected) {
      renderer.fillRect(tileX, tileY, tileWidth, metrics.menuRowHeight);
    } else {
      renderer.drawRect(tileX, tileY, tileWidth, metrics.menuRowHeight);
    }

    const int textWidth =
        renderer.getTextWidth(UI_10_FONT_ID, menuItems[i].label, textStyle);
    const int textX = (pageWidth - textWidth) / 2;
    const int textY =
        tileY +
        (metrics.menuRowHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    const bool blackText = emphasized ? selected : !selected;
    renderer.drawText(UI_10_FONT_ID, textX, textY, menuItems[i].label,
                      blackText, textStyle);
  }

  // Show "Remove" hint on Back button when a recent book is selected
  const bool isRecentBookSelected =
      selectorIndex >= 1 && selectorIndex <= static_cast<int>(recentBooks.size()) &&
      !recentBooks[selectorIndex - 1].path.empty();
  const char *backLabel = isRecentBookSelected ? "Remove" : "";
  const auto labels = mappedInput.mapLabels(backLabel, tr(STR_SELECT), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3,
                      labels.btn4);

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  }
}
