// Pre-allocation heap-probe utility.
//
// The firmware is compiled `-fno-exceptions`. Any std::vector::reserve(),
// std::vector::resize(), std::string::reserve(), or `new T(...)` that
// cannot satisfy the allocation under ESP32-C3 fragmentation calls
// __throw_bad_alloc -> std::terminate -> abort -> RTC_SW_SYS_RST. The
// recovery screens, silent-restart paths, and OOM logging never run.
//
// For the `new` form, the call site can use `new (std::nothrow) T(...)`
// and check for nullptr — covered in the memory-hardening branch sweep.
//
// For std::vector / std::string growth there is no nothrow variant, so
// the only safe form is to probe the heap first and bail before issuing
// the reserve / resize call. This header consolidates that probe.
//
// Usage:
//   if (!crosspoint::heap::canAllocateContiguous(bytes)) {
//     LOG_ERR(tag, "OOM ...");
//     return false;  // caller routes to recovery screen
//   }
//   v.reserve(n);   // safe — heap had the room a moment ago
//
// The probe value is heap_caps_get_largest_free_block(MALLOC_CAP_8BIT).
// The default headroom of 4 KB matches the wallpaper playlist V2 probe
// (RFC #156): empirically the smallest cushion that absorbs the
// transient growth between the probe and the reserve completing on a
// ESP32-C3 hot heap.
//
// Host-test builds compile with `UNIT_TEST_HOST=1` and have no
// heap_caps_*. The host shim returns SIZE_MAX so every probe passes —
// scenarios that need to *simulate* fragmentation drive an explicit
// override via setLargestFreeBlockOverride() (see test_memory_harness).
#pragma once

#include <cstddef>

#ifndef UNIT_TEST_HOST
#include <esp_heap_caps.h>
#endif

namespace crosspoint {
namespace heap {

constexpr size_t kDefaultHeadroomBytes = 4 * 1024;

#ifdef UNIT_TEST_HOST
// Host shim. Tests override via setLargestFreeBlockOverride() below.
inline size_t& largestFreeBlockOverride_() {
  static size_t v = SIZE_MAX;
  return v;
}
inline void setLargestFreeBlockOverride(size_t bytes) { largestFreeBlockOverride_() = bytes; }
inline void clearLargestFreeBlockOverride() { largestFreeBlockOverride_() = SIZE_MAX; }
inline size_t largestFreeBlockBytes() { return largestFreeBlockOverride_(); }
#else
inline size_t largestFreeBlockBytes() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}
#endif

// Returns true if the heap reports a contiguous free block large enough
// for `needBytes + headroomBytes`. Headroom defaults to 4 KB to absorb
// transient growth between the probe and the subsequent allocation.
inline bool canAllocateContiguous(size_t needBytes,
                                  size_t headroomBytes = kDefaultHeadroomBytes) {
  const size_t requested = needBytes + headroomBytes;
  // Overflow check: if requested wrapped, treat as unsatisfiable.
  if (requested < needBytes) return false;
  return largestFreeBlockBytes() >= requested;
}

}  // namespace heap
}  // namespace crosspoint
