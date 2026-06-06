#pragma once

// Single source of truth for "leave a WiFi activity" teardown.
//
// Six WiFi-using activities used to copy-paste the same disconnect -> settle ->
// WIFI_OFF -> settle -> guarded-silentRestart dance, with inconsistent delays
// (30/50/100 ms) and two different guard styles. Forgetting the guard is a
// silent ~50 KB leak that persists until the next manual reboot (it bit
// QRShare and KOReaderAuth before they were fixed by hand). This module hides
// the timing and the restart dispatch so no call site can get it wrong.
//
// Why the caller passes `wifiWasUp` instead of this module computing it:
//   - WiFi.getMode() is unreliable once WIFI_OFF has been set, so "did the
//     radio come up this session?" MUST be sampled before any teardown.
//   - A subactivity's onExit() (e.g. WifiSelectionActivity) can itself power
//     the radio down, so the sample must also precede the base-class onExit().
//   - The predicate genuinely varies: most sites read the radio state, but
//     KOReaderSync keys off its own session flag (state != NO_CREDENTIALS).
// Capture the bool at the very top of onExit(), then call this last.

namespace net {

enum class WifiRestartTarget {
  Home,    // silentRestart()         -> home screen
  Reader,  // silentRestartToReader() -> currently-open EPUB
};

// Gracefully brings the radio down (softAPdisconnect when `apMode`, otherwise a
// credential-preserving STA disconnect) with the settle time LWIP needs to
// flush, then — only if `wifiWasUp` — silent-restarts to reclaim the
// non-coalescing LWIP/netif scatter (see SilentRestart.h).
//
// Does NOT return when `wifiWasUp` is true.
void teardownAndReclaim(bool wifiWasUp,
                        WifiRestartTarget target = WifiRestartTarget::Home,
                        const char* reason = "wifi-exit",
                        bool apMode = false);

}  // namespace net
