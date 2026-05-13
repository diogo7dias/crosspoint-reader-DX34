#pragma once

#include "activities/ActivityWithSubactivity.h"
#include "network/OtaUpdater.h"

class OtaUpdateActivity : public ActivityWithSubactivity {
  enum State {
    WIFI_SELECTION,
    CHECKING_FOR_UPDATE,
    WAITING_CONFIRMATION,
    UPDATE_IN_PROGRESS,
    NO_UPDATE,
    FAILED,
    FINISHED,
    SHUTTING_DOWN
  };

  // Can't initialize this to 0 or the first render doesn't happen
  static constexpr unsigned int UNINITIALIZED_PERCENTAGE = 111;

  static constexpr int OTA_CHECK_MAX_RETRIES = 2;

  const std::function<void()> goBack;
  State state = WIFI_SELECTION;
  unsigned int lastUpdaterPercentage = UNINITIALIZED_PERCENTAGE;
  OtaUpdater updater;

  // Typed outcome carriers (RFC #146). Populated during the check / install
  // phases; rendered by switching on tag. .tag default values mean
  // "no failure observed yet" so the FAILED render can safely inspect both.
  CheckOutcome lastCheck;
  InstallOutcome lastInstall;
  bool lastInstallPresent = false;
  // Live progress bytes — updated from inside the install ProgressFn so the
  // render path no longer polls a getRender() flag plus two getter sizes.
  uint32_t progressBytesDone = 0;
  uint32_t progressBytesTotal = 1;  // 1 to avoid div-by-zero before first chunk

  // Heap reservation: a block held before WiFi starts, freed before TLS.
  // Prevents WiFi stack from fragmenting the region mbedTLS needs.
  void* tlsHeapReservation = nullptr;

  void onWifiSelectionComplete(bool success);

 public:
  explicit OtaUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             const std::function<void()>& goBack)
      : ActivityWithSubactivity("OtaUpdate", renderer, mappedInput), goBack(goBack), updater() {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;
  bool preventAutoSleep() override { return state == CHECKING_FOR_UPDATE || state == UPDATE_IN_PROGRESS; }
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode
};
