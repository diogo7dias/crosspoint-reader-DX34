#pragma once

#include <Epub.h>

#include <functional>
#include <memory>

#include "../ActivityWithSubactivity.h"
#include "BookmarkStore.h"
#include "util/ButtonNavigator.h"

/// Displays a scrollable list of bookmarks for the current book.
/// Select jumps to that position; long-press opens a Rename / Delete popup.
class BookmarkListActivity final : public ActivityWithSubactivity {
 public:
  explicit BookmarkListActivity(
      GfxRenderer& renderer, MappedInputManager& mappedInput,
      const std::shared_ptr<Epub>& epub, BookmarkStore& store,
      const std::string& cachePath, int currentSpineIndex, int currentPage,
      const std::function<void()>& onGoBack,
      const std::function<void(int spineIndex, int pageNumber)>& onJump)
      : ActivityWithSubactivity("BookmarkList", renderer, mappedInput),
        epub(epub),
        store(store),
        cachePath(cachePath),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        onGoBack(onGoBack),
        onJump(onJump) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  std::shared_ptr<Epub> epub;
  BookmarkStore& store;
  std::string cachePath;
  int currentSpineIndex;
  int currentPage;
  int selectorIndex = 0;

  ButtonNavigator buttonNavigator;

  const std::function<void()> onGoBack;
  const std::function<void(int spineIndex, int pageNumber)> onJump;

  static constexpr int kLineHeight = 30;

  // Inline action popup (shown on hold). Lets the user pick between
  // Rename, Delete, or Cancel for the selected bookmark.
  bool actionPopupOpen = false;
  int actionPopupBookmarkIndex = -1;
  int actionPopupSelectedIndex = 0;

  void openActionPopup(int bookmarkIndex);
  void executeBookmarkAction();
  void openKeyboardForRename(int bookmarkIndex);
  void openDeleteConfirm(int bookmarkIndex);
  void renderActionPopup();

  /// Build a display label for a bookmark (chapter name + page).
  std::string formatBookmark(const BookmarkStore::Bookmark& bm) const;
};
