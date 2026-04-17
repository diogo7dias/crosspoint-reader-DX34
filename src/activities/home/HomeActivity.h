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
  int scrollOffset = 0;       // First visible book index in recents list
  int firstVisibleBookIdx = 0; // Updated by renderer each frame
  int lastVisibleBookIdx = 0;  // Updated by renderer each frame
  bool recentsLoading = false;
  bool recentsLoaded = false;
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
