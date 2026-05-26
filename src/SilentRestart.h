#pragma once

#include <cstdint>

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a Wi-Fi session — WiFi/LWIP/netif
// teardown scatters long-lived allocations across the heap, leaving ~50 KB
// of contiguous space that's unrecoverable without a reboot. Ported from
// upstream crosspoint-reader 7acc31b (PR #1908).

// `reason` is a short stable label written to /heap_report.txt before the
// reboot. Pass a string literal — it is not copied. Examples:
// "wifi-exit-FileTransfer", "reader-oom-recovery", "ota-fail-recovery".
void silentRestart(const char* reason = "unspecified");          // home screen
void silentRestartToReader(const char* reason = "unspecified");  // currently-open EPUB (APP_STATE.openEpubPath)

// Read+clear semantics for setup(). Returns the requested target if a silent
// reboot is in progress, otherwise -1. Clears the flag atomically so a panic
// later in setup() can't loop us back into a silent reboot.
//
// Targets: 0 = home, 1 = reader.
int consumeSilentRebootTarget();

// Loop guard for caller-initiated auto-restart paths (e.g. reader heap
// fragmentation recovery). The first two consecutive silent restarts return
// true and the caller proceeds to call silentRestart*(). After that, this
// returns false and the caller must fall through to user-facing recovery
// rather than reboot-looping. Counter resets either on cold boot (RTC magic
// mismatch) or when the caller explicitly calls clearSilentRestartLoopGuard()
// after the system reaches a known-good state.
constexpr uint8_t kMaxConsecutiveAutoRestarts = 2;
bool tryReserveAutoSilentRestart();
void clearSilentRestartLoopGuard();
