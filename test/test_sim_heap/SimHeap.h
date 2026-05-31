// SimHeap — host-side allocation-failure injector for memory-stability sims.
//
// Why this exists (vs the existing FakeHeap):
//   FakeHeap is a *numbers* model: it answers heap_caps_get_largest_free_block()
//   probes so probe-guarded code (canAllocateContiguous) can be tested. It does
//   NOT intercept real new/malloc, so it cannot catch *unguarded* allocations —
//   which are exactly the ones that abort the firmware. The render-OOM bug
//   (FontDecompressor hot-group malloc failing on a fragmented heap) is invisible
//   to FakeHeap.
//
// SimHeap replaces the global operator new/delete on the host so that, while
// "armed", it models an ESP32-C3 non-moving fragmented heap and FAILS
// allocations that the device could not satisfy. The failure is delivered the
// same way the device experiences it:
//   - throwing operator new   -> throws std::bad_alloc. The firmware builds with
//                                -fno-exceptions, so on-device this is
//                                __throw_bad_alloc -> std::terminate -> abort ->
//                                RTC_SW_SYS_RST. In the sim a test catches it and
//                                treats it as "this allocation WOULD crash the
//                                firmware" (wouldAbortThrows()).
//   - nothrow operator new     -> returns nullptr, modelling the correct
//                                `new (std::nothrow) T` path the hardening sweep
//                                installs at safe call sites.
//
// The distinction between "threw (would-abort)" and "returned null (survived)"
// is the core memory-stability metric: a stable firmware must reach OOM only
// through nothrow/probe-guarded paths, never through a throwing allocation.
//
// Fragmentation model: a single `cap` = the largest contiguous free block. Any
// single allocation larger than `cap` fails regardless of total free — this is
// the non-moving-heap reality (freed transient blocks do not re-form the big
// contiguous run). `totalBudget` additionally models outright exhaustion.
//
// Host-only. Compiled into a test binary that wants allocation-failure
// injection; the global operator new/delete definitions live in SimHeap.cpp so
// there is exactly one replacement per binary.
#pragma once

#include <cstddef>
#include <cstdint>

namespace crosspoint {
namespace test {

class SimHeap {
 public:
  // Arm the model. `cap` = largest contiguous free block (fragmentation
  // ceiling); `totalBudget` = total heap size (exhaustion ceiling). While
  // armed, allocations exceeding either fail. Resets live/stat counters.
  static void arm(size_t cap, size_t totalBudget);
  // Stop modelling — operator new/delete pass straight through to malloc/free.
  static void disarm();
  static bool armed();

  // Adjust the fragmentation ceiling mid-run (e.g. to crumble the heap during
  // a render pass). Does not touch liveBytes.
  static void setCap(size_t cap);
  static size_t cap();

  static void reset();  // disarm + zero all counters

  // ---- Stats ----
  static unsigned attempts();          // total allocation requests while armed
  static unsigned fragFails();         // failed: request > cap
  static unsigned exhaustFails();      // failed: liveBytes + request > budget
  static unsigned wouldAbortThrows();  // throwing-new failures (device abort!)
  static unsigned nothrowNulls();      // nothrow-new failures (survived)
  static size_t liveBytes();
  static size_t peakLiveBytes();

  // ---- Internal: called by the global operator new/delete in SimHeap.cpp ----
  // Returns a user pointer or nullptr. `isNothrow` selects null-vs-throw on
  // failure (the throw is performed by the operator, not here, so this header
  // stays exception-agnostic).
  static void* allocate(size_t size, bool isNothrow, bool* outShouldThrow);
  static void deallocate(void* userPtr);

 private:
  static inline bool armed_ = false;
  static inline size_t cap_ = 0;
  static inline size_t totalBudget_ = 0;
  static inline size_t liveBytes_ = 0;
  static inline size_t peakLive_ = 0;
  static inline unsigned attempts_ = 0;
  static inline unsigned fragFails_ = 0;
  static inline unsigned exhaustFails_ = 0;
  static inline unsigned wouldAbortThrows_ = 0;
  static inline unsigned nothrowNulls_ = 0;
};

}  // namespace test
}  // namespace crosspoint
