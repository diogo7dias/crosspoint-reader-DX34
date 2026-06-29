// DeferredActionQueue — a cross-task deferred-action primitive (RFC #167).
//
// Replaces the hand-rolled `pending*` bool flags that defer an action from the
// render task (or a delegated subActivity->loop()) to the loop task to avoid
// use-after-free. A callback posts an action from any context; the loop task
// drains it.
//
// Templated on the activity's OWN `enum class : uint8_t` whose last enumerator
// is `Count` (so no firmware-wide god-enum couples unrelated activities). The
// enumerator VALUE is the drain priority (lowest drains first), which
// reproduces the old if-block order (e.g. GoHome before SectionReset).
// Coalescing by kind (a second post before a drain collapses to one) preserves
// the bool-flag "set twice = once" semantics. Payloads stay as typed members on
// the activity, written next to post() — zero added heap, overwrite-wins data
// semantics exactly as today.
//
// Pure: depends only on <cstdint> + DeferredActionGuard (a no-op on host), so
// the coalescing / priority / clear-before-act contract is host-unit-testable
// without FreeRTOS — the seam the old flags left untested.
#pragma once

#include <cstdint>

#include "DeferredActionGuard.h"

namespace crosspoint {

template <typename Action>
class DeferredActionQueue {
  static_assert(static_cast<uint32_t>(Action::Count) <= 32u,
                "DeferredActionQueue supports <= 32 actions (bitset coalescing)");

 public:
  // Post from ANY context (render task / subactivity loop). Coalesces by kind.
  // Guarded read-modify-write against a concurrent drain.
  void post(Action a) {
    DeferredActionGuard g;
    pending_ = static_cast<uint32_t>(pending_ | actionBit(a));
  }

  // Loop-task drain, in enum-order priority. `run(Action)` returns true to STOP
  // draining (the old `return;` after a navigation), false to continue (the old
  // fall-through, e.g. SectionReset). The bit is cleared under the guard BEFORE
  // run() is invoked, so a follow-up post() of the same kind re-arms cleanly for
  // the next pass rather than looping within this one. Returns true if an action
  // stopped the drain.
  template <typename Run>
  bool drain(Run&& run) {
    for (uint8_t i = 0; i < static_cast<uint8_t>(Action::Count); ++i) {
      const uint32_t mask = static_cast<uint32_t>(1u) << i;
      {
        DeferredActionGuard g;
        if ((pending_ & mask) == 0u) continue;
        pending_ = static_cast<uint32_t>(pending_ & ~mask);
      }
      if (run(static_cast<Action>(i))) return true;
    }
    return false;
  }

  // Non-mutating gate read (for render()'s "bail while X is queued" checks).
  bool pending(Action a) const { return (pending_ & actionBit(a)) != 0u; }

  // Explicit single-kind clear (for sites that consumed a flag without draining).
  void clear(Action a) {
    DeferredActionGuard g;
    pending_ = static_cast<uint32_t>(pending_ & ~actionBit(a));
  }

 private:
  // Named actionBit (not bit) to avoid clashing with the Arduino core's
  // `#define bit(b)` macro when this header is compiled into a firmware TU.
  static uint32_t actionBit(Action a) { return static_cast<uint32_t>(1u) << static_cast<uint8_t>(a); }

  volatile uint32_t pending_ = 0;
};

}  // namespace crosspoint
