#pragma once

#include <cstdint>

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a Wi-Fi session — WiFi/LWIP/netif
// teardown scatters long-lived allocations across the heap, leaving ~50 KB
// of contiguous space that's unrecoverable without a reboot. Ported from
// upstream crosspoint-reader 7acc31b (PR #1908).

void silentRestart();          // home screen
void silentRestartToReader();  // currently-open EPUB (APP_STATE.openEpubPath)

// Read+clear semantics for setup(). Returns the requested target if a silent
// reboot is in progress, otherwise -1. Clears the flag atomically so a panic
// later in setup() can't loop us back into a silent reboot.
//
// Targets: 0 = home, 1 = reader.
int consumeSilentRebootTarget();
