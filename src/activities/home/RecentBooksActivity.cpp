#include "RecentBooksActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookProgress.h"
#include "util/StringUtils.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;

std::string buildAuthorInitials(const std::string &author) {
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

} // namespace

void RecentBooksActivity::loadRecentBooks() {
  recentBooks.clear();
  const auto &books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(books.size());
  std::unordered_set<std::string> seenPaths;
  seenPaths.reserve(books.size());

  for (const auto &book : books) {
    // Hide QUOTES sidecar files from the recents list
    if (book.path.size() >= 11 &&
        book.path.compare(book.path.size() - 11, 11, "_QUOTES.txt") == 0) {
      continue;
    }

    // Skip if file no longer exists
    if (!Storage.exists(book.path.c_str())) {
      continue;
    }
    const auto percent = BookProgress::getPercent(book.path);
    if (!percent.has_value() || percent.value() <= 2) {
      continue;
    }

    if (!seenPaths.insert(book.path).second) {
      continue;
    }

    RecentBook decorated = book;
    const std::string initials = buildAuthorInitials(book.author);
    const std::string titleWithAuthor =
        initials.empty() ? book.title : (book.title + " by " + initials);
    decorated.title = "[" + std::to_string(percent.value()) + "%]  " + titleWithAuthor;
    recentBooks.push_back(std::move(decorated));
  }
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  // Load data
  loadRecentBooks();

  selectorIndex = 0;
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
}

void RecentBooksActivity::loop() {
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(
      renderer, true, false, true, true);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!recentBooks.empty() &&
        selectorIndex < static_cast<int>(recentBooks.size())) {
      LOG_DBG("RBA", "Selected recent book: %s",
              recentBooks[selectorIndex].path.c_str());
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
  }

  int listSize = static_cast<int>(recentBooks.size());

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex =
        ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(
        static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(
        static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(
        static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

void RecentBooksActivity::render(Activity::RenderLock &&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer,
                 Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_MENU_RECENT_BOOKS));

  const int contentTop =
      metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop -
                            metrics.buttonHintsHeight - metrics.verticalSpacing;

  // Recent tab
  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding,
                      contentTop + 20, tr(STR_NO_RECENT_BOOKS));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight},
        recentBooks.size(), selectorIndex,
        [this](int index) { return recentBooks[index].title; },
        [](int) { return std::string(); }, nullptr, nullptr);
  }

  // Help text
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN),
                                            tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3,
                      labels.btn4);

  renderer.displayBuffer();
}
