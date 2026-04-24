#pragma once

#include <atomic>
#include <string>

#include "activities/Activity.h"
#include "BdfIndexBuilder.h"
#include <functional>

// Full-screen progress activity shown while BdfIndexBuilder walks a
// large (9–12 MB) BDF file. Updated live from the builder's progress
// callback — the build runs on the main loop task while the activity's
// render task picks up requestUpdate() notifications and repaints the
// bar.
//
// Updates are rate-limited by the e-ink refresh (~200 ms / frame); the
// builder can call setProgress() far more often and any intermediate
// updates are coalesced.
class CustomFontInstallProgressActivity final : public Activity {
 public:
  CustomFontInstallProgressActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filename, std::string bdfPath, std::string idxPath, std::function<void(crosspoint::bdf::BuildIndexResult)> onComplete)
      : Activity("CustomFontInstallProgress", renderer, mappedInput), 
        filename(std::move(filename)),
        bdfPath(std::move(bdfPath)),
        idxPath(std::move(idxPath)),
        onComplete(std::move(onComplete)) {}

  void onEnter() override;
  void render(Activity::RenderLock&&) override;
  void loop() override;
  bool preventAutoSleep() override { return true; }

  // Thread-safe: caller may be on the main loop, render task reads
  // atomics. Paired requestUpdate() from the caller kicks the render.
  void setProgress(uint32_t done, uint32_t total) {
    progressDone.store(done, std::memory_order_relaxed);
    progressTotal.store(total, std::memory_order_relaxed);
  }

 private:
  std::string filename;
  std::string bdfPath;
  std::string idxPath;
  std::function<void(crosspoint::bdf::BuildIndexResult)> onComplete;

  std::atomic<bool> taskDone{false};
  crosspoint::bdf::BuildIndexResult taskResult{};

  std::atomic<uint32_t> progressDone{0};
  std::atomic<uint32_t> progressTotal{0};
};
