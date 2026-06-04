#pragma once

#include "activities/ActivityWithSubactivity.h"

// RFC #160: on-device TLS OTA was unreliable on this device's fragmented heap
// (mbedTLS needs ~10 KB contiguous during the handshake, which the C3's tight
// heap rarely has) — the reason firmware updates moved to the browser. This
// screen no longer downloads anything; it points the user at the browser
// /update flow and shows the current version. Reached from Settings -> Check
// for updates.
class OtaUpdateActivity : public ActivityWithSubactivity {
  const std::function<void()> goBack;

 public:
  explicit OtaUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             const std::function<void()>& goBack)
      : ActivityWithSubactivity("OtaUpdate", renderer, mappedInput), goBack(goBack) {}
  void onEnter() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;
};
