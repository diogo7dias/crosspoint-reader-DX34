#pragma once

#include <cstddef>
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

// Read-only peek at how many auto-restarts remain in this cold-boot session,
// without reserving one. The recovery-ladder decision (crosspoint::mem::
// nextRecoveryStep) is a pure function of remaining budget, so it needs the
// count up front; the caller still calls tryReserveAutoSilentRestart() to
// actually claim a slot when the ladder lands on SilentRestart. Does not
// mutate the guard (no magic init), unlike tryReserveAutoSilentRestart().
uint8_t remainingAutoSilentRestarts();

// ── Pre-restart hooks ───────────────────────────────────────────────────────
// Run, in registration order, inside EVERY silentRestart*/armAndRestart path
// immediately before ESP.restart(), so a silent reboot can never silently drop
// pending durable state. The motivating gap: most silent-restart call sites
// (WiFi-session exits, OOM-on-activity-entry) reboot without flushing, so a
// debounced write queued just before the restart (e.g. KOReader credentials)
// was lost. Register one hook at boot that flushes all dirty persist stores
// (PersistManager().flushAll()) and the durability becomes structural — no call
// site can forget. Plain function pointers (no heap, callback-safe), fixed
// capacity, matching the MemoryPolicy ShedFn idiom. Header-only so the registry
// + runner are host-testable without pulling the ESP-only armAndRestart in.
using PreRestartHook = void (*)();
inline constexpr size_t kMaxPreRestartHooks = 4;

namespace detail {
struct PreRestartRegistry {
  PreRestartHook fns[kMaxPreRestartHooks];
  size_t count;
};
inline PreRestartRegistry& preRestartRegistry() {
  static PreRestartRegistry r{};
  return r;
}
}  // namespace detail

inline void registerPreRestartHook(PreRestartHook fn) {
  detail::PreRestartRegistry& r = detail::preRestartRegistry();
  if (fn && r.count < kMaxPreRestartHooks) {
    r.fns[r.count++] = fn;
  }
}

// Invoked by armAndRestart before the reboot. Safe to call standalone in tests.
inline void runPreRestartHooks() {
  detail::PreRestartRegistry& r = detail::preRestartRegistry();
  for (size_t i = 0; i < r.count; ++i) {
    r.fns[i]();
  }
}

inline void clearPreRestartHooksForTest() { detail::preRestartRegistry().count = 0; }
