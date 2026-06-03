#include "SilentRestart.h"

#include <Arduino.h>
#include <Logging.h>
#include <esp_attr.h>

#include "HeapReport.h"

namespace {

// RTC_NOINIT memory survives ESP.restart() but not a hard power loss.
// Uninitialized on cold boot — magic-word check below filters that case.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;

// Separate magic + counter for the auto-restart loop guard. Survives every
// silent reboot so we can bound the number of consecutive automatic
// restart attempts before we must fall through to a user-facing recovery.
RTC_NOINIT_ATTR uint32_t autoRestartLoopMagic;
RTC_NOINIT_ATTR uint32_t autoRestartLoopCount;

constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;
constexpr uint32_t AUTO_RESTART_LOOP_MAGIC = 0xC1EA1009;

void armAndRestart(uint32_t target, const char* label, const char* reason) {
  silentRebootTarget = target;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  // LOG_DIAG (not LOG_DBG): silent restarts are rare events tied to WiFi
  // session exits, and seeing them in the RTC ring is genuinely useful for
  // diagnosing future fragmentation regressions even on production builds.
  LOG_DIAG("MAIN", "Silent restart (target=%s reason=%s)", label, reason ? reason : "(none)");
  // Dump /heap_report.txt before rebooting. Best-effort — failure is logged
  // by writeHeapReport itself, but never blocks the restart.
  writeHeapReport(reason);
  // 50 ms is upstream's value — gives the LOG line time to drain to USB CDC
  // and the WiFi disconnect frame above this call time to leave the radio.
  delay(50);
  ESP.restart();
}

}  // namespace

void silentRestart(const char* reason) { armAndRestart(SILENT_REBOOT_TARGET_HOME, "home", reason); }

void silentRestartToReader(const char* reason) { armAndRestart(SILENT_REBOOT_TARGET_READER, "reader", reason); }

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
  // Cold boot also resets the auto-restart loop guard. If the reboot was a
  // hard power cycle, RTC_NOINIT contents are garbage and the magic check
  // in tryReserveAutoSilentRestart catches it; if it was a panic-induced
  // reset (not a silentRestart call), the user-facing crash report path
  // ran and they've already seen something gone wrong — we reset so the
  // next genuine fragmentation-recovery cycle starts with a fresh budget.
  if (!isSilentReboot) {
    autoRestartLoopMagic = AUTO_RESTART_LOOP_MAGIC;
    autoRestartLoopCount = 0;
  }
  return isSilentReboot ? static_cast<int>(target) : -1;
}

bool tryReserveAutoSilentRestart() {
  if (autoRestartLoopMagic != AUTO_RESTART_LOOP_MAGIC) {
    // First use this cold-boot session.
    autoRestartLoopMagic = AUTO_RESTART_LOOP_MAGIC;
    autoRestartLoopCount = 0;
  }
  if (autoRestartLoopCount >= kMaxConsecutiveAutoRestarts) {
    LOG_DIAG("MAIN", "auto-restart guard: count=%u at cap (%u) - falling through to user recovery",
             (unsigned)autoRestartLoopCount, (unsigned)kMaxConsecutiveAutoRestarts);
    return false;
  }
  autoRestartLoopCount++;
  LOG_DIAG("MAIN", "auto-restart guard: reserved attempt %u/%u", (unsigned)autoRestartLoopCount,
           (unsigned)kMaxConsecutiveAutoRestarts);
  return true;
}

void clearSilentRestartLoopGuard() {
  autoRestartLoopMagic = AUTO_RESTART_LOOP_MAGIC;
  autoRestartLoopCount = 0;
}

uint8_t remainingAutoSilentRestarts() {
  // Magic not yet set this cold-boot session = full budget untouched. Mirror
  // tryReserveAutoSilentRestart's first-use semantics WITHOUT writing the
  // magic, so this stays a pure read (the actual reserve does the init).
  if (autoRestartLoopMagic != AUTO_RESTART_LOOP_MAGIC) {
    return kMaxConsecutiveAutoRestarts;
  }
  if (autoRestartLoopCount >= kMaxConsecutiveAutoRestarts) {
    return 0;
  }
  return static_cast<uint8_t>(kMaxConsecutiveAutoRestarts - autoRestartLoopCount);
}
