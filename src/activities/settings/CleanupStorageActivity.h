#pragma once

#include <functional>

#include "activities/ActivityWithSubactivity.h"

// Settings action that scans the SD card for ephemeral cruft and removes
// the parts that are safe to drop: orphan atomic-write tmp/tmp2/junk-*
// files, the pre-RFC-#146 /wifi_report.txt, and over-large /diag_report.txt,
// /crash_report.txt, or /heap_report.txt rotation candidates.
//
// Preserves: books, /.crosspoint/* primary stores (settings, state, recent,
// themes), per-book progress/bookmarks, custom fonts, sleep wallpapers,
// every .bak file (those are the rollback half of the 2-layer atomic-write
// chain and are NEVER orphans).
class CleanupStorageActivity final : public ActivityWithSubactivity {
 public:
  explicit CleanupStorageActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                  const std::function<void()>& goBack)
      : ActivityWithSubactivity("CleanupStorage", renderer, mappedInput), goBack(goBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  enum State { WARNING, CLEANING, SUCCESS, NOTHING_TO_DO, FAILED };

  State state = WARNING;
  const std::function<void()> goBack;

  int removedCount = 0;
  int failedCount = 0;
  uint32_t bytesFreed = 0;

  void runCleanup();
};
