// BootSequenceOrchestrator — the boot-destination decision as a pure module
// (RFC #166, step 1).
//
// Today main.cpp:806-865 computes (readerPath, goHome) inline inside the
// 289-line setup(), reading five globals (APP_STATE, RECENT_BOOKS, SETTINGS,
// mappedInputManager, RTC) + random(), and performing a durable side effect.
// That includes the brick-class crash-loop guard (readerActivityLoadCount): an
// OOM-on-open book that re-launches forever unless the guard routes a boot to
// the library. None of it is host-testable.
//
// This core takes a BootInputs snapshot + an injected random (a function
// pointer the core calls with the FILTERED epub count, so the caller never
// re-derives the .epub filter) and returns a BootDecision. It performs NO I/O:
// the durable guard bump (clear openEpubPath; increment readerActivityLoadCount;
// saveToFileSync) is RETURNED as the bumpGuard flag and applied by the caller.
//
// Includes only <cstdint>/<string>/<vector> — no Arduino/FreeRTOS/HAL/globals —
// so it compiles and unit-tests on the host. Step 1 is additive: nothing is
// wired into setup() yet, so device behaviour is unchanged. The branches are a
// line-for-line transcription of main.cpp:812-864.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace crosspoint {
namespace boot {

// Snapshot of every impure input, gathered by the caller from the globals.
struct BootInputs {
  bool isSilentReboot = false;
  int silentRebootTarget = -1;               // consumeSilentRebootTarget(): -1 none, 0 home, 1 reader
  std::string openEpubPath;                  // APP_STATE.openEpubPath (read)
  uint8_t readerActivityLoadCount = 0;       // APP_STATE crash-loop guard (read)
  bool backHeld = false;                     // mappedInputManager.isPressed(Back)
  std::vector<std::string> recentBookPaths;  // RECENT_BOOKS paths, in order, UNFILTERED
  bool randomBookOnBoot = false;             // SETTINGS.randomBookOnBoot
};

// Count-dependent random, injected. The core calls it with the FILTERED epub
// count (which only the core knows). Plain function pointer — no std::function,
// no vtable, no heap.
using RandomIndexFn = uint32_t (*)(uint32_t count);  // returns [0, count)

struct BootDecision {
  bool goHome = true;      // true -> RouteId::Home, false -> RouteId::Reader
  std::string readerPath;  // valid iff !goHome
  // When true the caller MUST, before launching the reader, run the durable
  // crash-loop guard side effect: clear openEpubPath; increment
  // readerActivityLoadCount (cap 255); APP_STATE.saveToFileSync(). Returned, not
  // performed, so the core stays pure. Always false for home + any silent boot.
  bool bumpGuard = false;
};

namespace detail {
// Exact legacy predicate (main.cpp:833): p.size() > 5 && rfind(".epub") == size-5.
inline bool endsWithEpub(const std::string& p) { return p.size() > 5 && p.rfind(".epub") == p.size() - 5; }
}  // namespace detail

// The whole decision, pure + deterministic given (inputs, rng).
inline BootDecision decideBoot(const BootInputs& in, RandomIndexFn rng) {
  BootDecision d;

  if (in.isSilentReboot) {
    // Silent reboot bypasses the crash-loop / recents / random logic. Target
    // was decided by the exiting activity; never bumps the guard.
    if (in.silentRebootTarget == 1 && !in.openEpubPath.empty()) {
      d.readerPath = in.openEpubPath;
      d.goHome = false;
    } else {
      d.goHome = true;
    }
    return d;
  }

  // Normal boot: skip straight to home if Back held or crash-loop detected.
  const bool forcedHome = in.backHeld || in.readerActivityLoadCount > 0;

  std::vector<const std::string*> recentEpubs;
  if (!forcedHome) {
    for (const auto& path : in.recentBookPaths) {
      if (detail::endsWithEpub(path)) recentEpubs.push_back(&path);
    }
  }

  if (!forcedHome && !recentEpubs.empty()) {
    if (in.randomBookOnBoot && recentEpubs.size() > 1) {
      const uint32_t idx = rng(static_cast<uint32_t>(recentEpubs.size()));
      d.readerPath = *recentEpubs[idx % recentEpubs.size()];  // defensive modulo
    } else {
      d.readerPath = *recentEpubs.front();
    }
  }

  d.goHome = d.readerPath.empty();
  d.bumpGuard = !d.goHome;  // a normal-boot reader launch must persist the guard
  return d;
}

}  // namespace boot
}  // namespace crosspoint
