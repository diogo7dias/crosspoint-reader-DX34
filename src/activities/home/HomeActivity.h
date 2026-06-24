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
  // Re-entry refresh: true when home is re-entered with a cached list still
  // in memory. The cached list is rendered immediately, and the SD scan runs
  // on the first loop tick to refresh in place. On cold boot the scan runs
  // synchronously inside onEnter() (covered by the "Loading home…" transition
  // popup) so the recents list is fully populated by the first frame.
  bool recentsStale = false;
  bool firstRenderDone = false;
  bool hasOpdsUrl = false;
  bool sleepFavoritesFull = false;
  size_t protectedSleepFavoriteCount = 0;
  // Over-limit warning card: shown as the first selectable item in the recents
  // area when /sleep holds more images than the rotation cap. Selecting it opens
  // the numeric keypad to bulk-move random images to /sleep pause.
  bool sleepOverLimit = false;
  long sleepImageCount = 0;
  std::string moveToast;  // transient result popup after a bulk move

  // Cached /sleep image count, shared across HomeActivity recreations so a home
  // re-entry doesn't rescan the (potentially large) folder on the snappy path.
  static long cachedSleepImageCount;
  static bool sleepImageCountKnown;
  // Static so the list survives HomeActivity destruction/recreation (each
  // home entry constructs a fresh instance). Lets re-entries render the
  // last-known recents immediately while the background rescan runs.
  static std::vector<RecentBook> recentBooks;
  const std::function<void(const std::string& path)> onSelectBook;
  const std::function<void()> onMyLibraryOpen;
  const std::function<void()> onRecentsOpen;
  const std::function<void()> onSettingsOpen;
  const std::function<void()> onFileTransferOpen;
  const std::function<void()> onOpdsBrowserOpen;

  int getRecentSlotCount() const;
  int getMenuItemCount() const;
  void refreshSleepFavoriteWarning();
  void maybeShowWallpaperPauseToast();
  void loadRecentBooks(int maxBooks);

  // Selector-model helpers. The warning card, when present, occupies
  // selectorIndex 1 (right after the pages-read tile), shifting books down.
  int warnSlots() const { return sleepOverLimit ? 1 : 0; }
  int firstBookSelector() const { return 1 + warnSlots(); }
  void refreshSleepOverLimit();
  void openSleepMoveKeypad();

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
