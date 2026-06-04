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

#include "PagedProgressSink.h"
#include "ReaderInputDispatcher.h"
#include "ReaderProgressTracker.h"
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
  // RFC #165: the Confirm tap/double-tap/long-press FSM now lives in the shared
  // ReaderInputDispatcher. chapterSkip=true maps the held page-button to a
  // 10-page jump; longPressConfirm maps to the orientation toggle.
  crosspoint::reader::ReaderInputDispatcher inputDispatcher_{
      crosspoint::reader::ReaderInputConfig{/*doubleTapToggle=*/true, /*longPressConfirm=*/true,
                                            /*footnoteBack=*/false, /*chapterSkip=*/true}};
  // RFC #171 step 0: progress persistence via the shared host-tested
  // ReaderProgressTracker + 4-byte PagedProgressSink (was hand-rolled
  // progressDirty/lastProgressChangeMs/lastObserved/lastSaved). Single-doc =>
  // ReaderPosition{0, (int32_t)currentPage, 1}.
  crosspoint::reader::PagedProgressSink progressSink_{"", "XTR"};
  crosspoint::reader::ReaderProgressTracker progress_{progressSink_};
  int recentSwitcherSelection = 0;
  std::vector<RecentBook> recentSwitcherBooks;

  void renderPage();
  void renderRecentSwitcher();
  void flushProgressIfNeeded(bool force);
  void loadProgress();
  void openChapterMenu();
  void toggleTextRenderMode();
  void toggleOrientation();
  void turnPages(int delta);  // +/- page jump with end-of-book + zero clamps (1 = page, 10 = skip)
  // RFC #165: snapshot MappedInputManager into the pure dispatcher frame, then
  // execute the decoded action. The FSM decision is host-tested in the core.
  crosspoint::reader::ReaderInput snapshotInput();
  void applyEffect(const crosspoint::reader::ReaderInputDispatcher::Result& effect);

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
