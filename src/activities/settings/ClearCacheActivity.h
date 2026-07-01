#pragma once

#include <cstdint>
#include <functional>

#include "activities/ActivityWithSubactivity.h"

class ClearCacheActivity final : public ActivityWithSubactivity {
 public:
  explicit ClearCacheActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                              const std::function<void()>& goBack)
      : ActivityWithSubactivity("ClearCache", renderer, mappedInput), goBack(goBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  enum State { WARNING, CLEARING, SUCCESS, FAILED };

  State state = WARNING;

  const std::function<void()> goBack;

  int clearedCount = 0;
  int failedCount = 0;
  uint64_t cacheBytes = 0;  // total bytes across all reading-cache dirs (scanned on enter)
  int cacheBooks = 0;       // number of cached books found
  void scanCacheSize();
  void clearCache();
};
