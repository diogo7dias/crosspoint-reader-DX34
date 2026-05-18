// Host-side virtual heap model for memory-stability tests.
//
// Simulates the ESP32-C3 non-moving heap so production code that probes
// `heap_caps_get_largest_free_block()` can be exercised under deterministic
// fragmentation conditions without flashing.
//
// Production callers inject `deps_.largestFreeBlockFn = []{ return
// heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT); }`. In tests we
// replace it with `FakeHeap::largestFreeBlock` and drive the underlying
// state via `FakeHeap::set*` methods.
//
// The model is intentionally minimal — it tracks three numbers (total
// free, largest contiguous block, min free observed) plus a list of
// scripted "allocations" that consume those numbers. It does NOT actually
// allocate memory; it only answers the questions the firmware asks of
// `heap_caps_*`.
//
// Scenarios reproduce the on-device crash signatures from TODO.md:
//   - Crash 1 (2026-05-17, book A): 14 KB request after defrag no-op, with
//     largest = 25588 but caps=6144 → fragmentation classified some pages
//     to a smaller cap.
//   - Crash 2 (2026-05-17, book B): 59 KB request, largest crumbles from
//     42996 → 6 KB during the section-load call.
#pragma once

#include <cstddef>
#include <cstdint>

namespace crosspoint {
namespace test {

class FakeHeap {
 public:
  // Reset all counters and state. Call at the start of each test.
  static void reset() {
    totalFree_ = 0;
    largestFreeBlock_ = 0;
    minFreeEver_ = 0;
    probeCount_ = 0;
    allocAttempts_ = 0;
    allocFailures_ = 0;
    lastFailedAllocBytes_ = 0;
  }

  // ---- Direct state setters (scenario authoring) -------------------------

  static void setTotalFree(size_t bytes) { totalFree_ = bytes; }
  static void setLargestFreeBlock(size_t bytes) {
    largestFreeBlock_ = bytes;
    if (minFreeEver_ == 0 || bytes < minFreeEver_) minFreeEver_ = bytes;
  }
  static void setMinFreeEver(size_t bytes) { minFreeEver_ = bytes; }

  // Simulate a fragmentation event: total free unchanged but the largest
  // contiguous block shrinks. Mirrors the on-device pattern where many
  // small frees accumulate without coalescing.
  static void fragment(size_t newLargest) {
    largestFreeBlock_ = newLargest;
    if (minFreeEver_ == 0 || newLargest < minFreeEver_) minFreeEver_ = newLargest;
  }

  // ---- Production-facing probes ------------------------------------------

  static size_t largestFreeBlock() {
    ++probeCount_;
    return largestFreeBlock_;
  }
  static size_t totalFree() { return totalFree_; }
  static size_t minFreeEver() { return minFreeEver_; }

  // Simulate an allocation request. Returns true if the heap can satisfy
  // the request (i.e. largestFreeBlock >= bytes). Decrements largest by
  // the request size and total by the same. Tracks failure counter.
  //
  // This is the host-side analogue of `new (std::nothrow) T(...)` —
  // production code calls it indirectly via the inline heap-probe
  // utility once that lands.
  static bool tryAllocate(size_t bytes) {
    ++allocAttempts_;
    if (bytes > largestFreeBlock_) {
      ++allocFailures_;
      lastFailedAllocBytes_ = bytes;
      return false;
    }
    largestFreeBlock_ -= bytes;
    if (bytes > totalFree_) totalFree_ = 0;
    else totalFree_ -= bytes;
    if (minFreeEver_ == 0 || largestFreeBlock_ < minFreeEver_) minFreeEver_ = largestFreeBlock_;
    return true;
  }

  // ---- Test inspection ---------------------------------------------------

  static unsigned probeCount() { return probeCount_; }
  static unsigned allocAttempts() { return allocAttempts_; }
  static unsigned allocFailures() { return allocFailures_; }
  static size_t lastFailedAllocBytes() { return lastFailedAllocBytes_; }

 private:
  static inline size_t totalFree_ = 0;
  static inline size_t largestFreeBlock_ = 0;
  static inline size_t minFreeEver_ = 0;
  static inline unsigned probeCount_ = 0;
  static inline unsigned allocAttempts_ = 0;
  static inline unsigned allocFailures_ = 0;
  static inline size_t lastFailedAllocBytes_ = 0;
};

// Scenario presets matching the on-device crash signatures.
struct HeapScenario {
  size_t totalFree;
  size_t largestFreeBlock;
  size_t minFreeEver;
  const char* label;
};

// Healthy heap right after boot, no books opened.
inline constexpr HeapScenario kHealthyBoot = {
    .totalFree = 200000,
    .largestFreeBlock = 90000,
    .minFreeEver = 90000,
    .label = "healthy-boot",
};

// Mid-reading after the section cache warms up — typical operating point.
inline constexpr HeapScenario kReaderMidSession = {
    .totalFree = 120000,
    .largestFreeBlock = 48000,
    .minFreeEver = 32000,
    .label = "reader-mid-session",
};

// Reproduces Crash 1 pre-conditions: total free is decent but the largest
// contiguous block has crumbled below the section-load floor.
inline constexpr HeapScenario kCrash1Preconditions = {
    .totalFree = 81000,
    .largestFreeBlock = 25588,
    .minFreeEver = 25588,
    .label = "crash1-pre",
};

// Reproduces Crash 2 pre-conditions: largest probe optimistic, but mid-call
// fragmentation crumbles it to a level that can't satisfy the 59 KB CSS reload.
inline constexpr HeapScenario kCrash2Preconditions = {
    .totalFree = 81512,
    .largestFreeBlock = 42996,
    .minFreeEver = 26596,
    .label = "crash2-pre",
};

inline void applyScenario(const HeapScenario& s) {
  FakeHeap::reset();
  FakeHeap::setTotalFree(s.totalFree);
  FakeHeap::setLargestFreeBlock(s.largestFreeBlock);
  FakeHeap::setMinFreeEver(s.minFreeEver);
}

}  // namespace test
}  // namespace crosspoint
