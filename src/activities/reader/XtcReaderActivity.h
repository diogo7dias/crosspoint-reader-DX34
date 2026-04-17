/**
 * XtcReaderActivity.h
 *
 * XTC ebook reader activity for CrossPoint Reader
 * Displays pre-rendered XTC pages on e-ink display
 */

#pragma once

#include <Xtc.h>
#include <cstdint>
#include <vector>

#include "activities/ActivityWithSubactivity.h"

struct RecentBook;

class XtcReaderActivity final : public ActivityWithSubactivity {
  std::shared_ptr<Xtc> xtc;

  uint32_t currentPage = 0;
  int pagesUntilFullRefresh = 0;

  const std::function<void()> onGoBack;
  const std::function<void()> onGoHome;
  const std::function<void(const std::string&)> onOpenBook;
  bool recentSwitcherOpen = false;
  bool pendingSingleBack = false;
  unsigned long lastBackReleaseMs = 0;
  bool confirmLongPressHandled = false;
  bool pendingMenuOpen = false;
  unsigned long lastConfirmReleaseMs = 0;
  bool progressDirty = false;
  unsigned long lastProgressChangeMs = 0;
  int32_t lastObservedPage = -1;
  int32_t lastSavedPage = -1;
  int recentSwitcherSelection = 0;
  std::vector<RecentBook> recentSwitcherBooks;

  void renderPage();
  void renderRecentSwitcher();
  void saveProgress() const;
  void flushProgressIfNeeded(bool force);
  void loadProgress();
  void openChapterMenu();
  void toggleTextRenderMode();

 public:
  explicit XtcReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Xtc> xtc,
                             const std::function<void()>& onGoBack, const std::function<void()>& onGoHome,
                             const std::function<void(const std::string&)>& onOpenBook)
      : ActivityWithSubactivity("XtcReader", renderer, mappedInput),
        xtc(std::move(xtc)),
        onGoBack(onGoBack),
        onGoHome(onGoHome),
        onOpenBook(onOpenBook) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;
};
