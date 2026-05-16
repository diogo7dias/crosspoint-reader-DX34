#include "SilentRestart.h"

#include <Arduino.h>
#include <Logging.h>
#include <esp_attr.h>

namespace {

// RTC_NOINIT memory survives ESP.restart() but not a hard power loss.
// Uninitialized on cold boot — magic-word check below filters that case.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;

constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;

void armAndRestart(uint32_t target, const char* label) {
  silentRebootTarget = target;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=%s)", label);
  // 50 ms is upstream's value — gives the LOG line time to drain to USB CDC
  // and the WiFi disconnect frame above this call time to leave the radio.
  delay(50);
  ESP.restart();
}

}  // namespace

void silentRestart() { armAndRestart(SILENT_REBOOT_TARGET_HOME, "home"); }

void silentRestartToReader() { armAndRestart(SILENT_REBOOT_TARGET_READER, "reader"); }

int consumeSilentRebootTarget() {
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  // Bound the target range — RTC_NOINIT is uninitialized on cold boot, so
  // even a matching magic word could pair with garbage in the target slot
  // if we're extremely unlucky. Clamp to a known value.
  const uint32_t target =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_READER) ? silentRebootTarget : 0;
  // Read-and-clear: even if setup() panics after this, the next boot won't
  // loop into a silent reboot. The user will see a normal boot splash.
  silentRebootMagic = 0;
  silentRebootTarget = 0;
  return isSilentReboot ? static_cast<int>(target) : -1;
}
