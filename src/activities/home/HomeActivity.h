#pragma once
#include <functional>
#include <string>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "./MyLibraryActivity.h"
#include "util/ButtonNavigator.h"

struct RecentBook;
struct Rect;

class HomeActivity final : public ActivityWithSubactivity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  int scrollOffset = 0;         // First visible book index in recents list
  int firstVisibleBookIdx = 0;  // Updated by renderer each frame
  int lastVisibleBookIdx = 0;   // Updated by renderer each frame
  // Paint-then-load: true between onEnter() and the first loop() tick that
  // runs the SD scan. While set, render() draws a "Loading recents…"
  // placeholder in the recents area and loop() bails out before any input
  // handling so the user can't act on a stale empty list.
  bool recentsLoading = false;
  bool firstRenderDone = false;
  bool hasOpdsUrl = false;
  bool sleepFavoritesFull = false;
  size_t protectedSleepFavoriteCount = 0;
  std::vector<RecentBook> recentBooks;
  const std::function<void(const std::string& path)> onSelectBook;
  const std::function<void()> onMyLibraryOpen;
  const std::function<void()> onRecentsOpen;
  const std::function<void()> onSettingsOpen;
  const std::function<void()> onFileTransferOpen;
  const std::function<void()> onOpdsBrowserOpen;

  int getRecentSlotCount() const;
  int getMenuItemCount() const;
  void refreshSleepFavoriteWarning();
  void loadRecentBooks(int maxBooks);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        const std::function<void(const std::string& path)>& onSelectBook,
                        const std::function<void()>& onMyLibraryOpen, const std::function<void()>& onRecentsOpen,
                        const std::function<void()>& onSettingsOpen, const std::function<void()>& onFileTransferOpen,
                        const std::function<void()>& onOpdsBrowserOpen)
      : ActivityWithSubactivity("Home", renderer, mappedInput),
        onSelectBook(onSelectBook),
        onMyLibraryOpen(onMyLibraryOpen),
        onRecentsOpen(onRecentsOpen),
        onSettingsOpen(onSettingsOpen),
        onFileTransferOpen(onFileTransferOpen),
        onOpdsBrowserOpen(onOpdsBrowserOpen) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;
};
