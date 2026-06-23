#pragma once

#include <functional>
#include <string>

#include "activities/Activity.h"
#include "sleep/SleepOrderPager.h"
#include "util/ButtonNavigator.h"

// Read-only viewer for the sleep-wallpaper rotation. Lists every wallpaper in
// /sleep in the order rotation will play them (the /.crosspoint/sleep_order.txt
// order), one screen at a time. It NEVER materializes the full list: each page
// is produced by streaming the order source line-by-line through
// readSleepOrderPage, so peak heap stays bounded by the page window regardless
// of folder size — the OOM that a whole-file read would trigger on a fragmented
// sleep-entry heap cannot happen here.
class SleepWallpaperListActivity final : public Activity {
 public:
  explicit SleepWallpaperListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                      const std::function<void()>& onBack)
      : Activity("SleepWallpaperList", renderer, mappedInput), onBack(onBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  // Stream the rotation order and collect only the entries in [start, start+count).
  // Prefers the order file (true play order); falls back to a folder scan (SD
  // order) when no order file exists yet. Sets `fromOrderFile` accordingly. Both
  // paths are O(1) heap beyond the returned page.
  crosspoint::sleep::OrderPage loadPage(size_t start, size_t count, bool& fromOrderFile) const;

  const std::function<void()> onBack;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  size_t totalCount = 0;
  bool orderedSource = false;  // true when the list reflects true rotation order

  static constexpr int kLineHeight = 30;
};
