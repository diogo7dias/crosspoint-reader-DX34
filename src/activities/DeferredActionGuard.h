// DeferredActionGuard — the one platform dependency of DeferredActionQueue
// (RFC #167), isolated behind UNIT_TEST_HOST like the rest of the codebase's
// shim idiom (HeapGuard, ActivityStubForHostTest).
//
// Consolidating ~20 independent single-word `pending*` bool writes into one
// shared bitset introduces a read-modify-write that the loose flags did not
// have: on the single-core ESP32-C3 the render task can preempt the loop task
// mid-update. A brief scheduler suspend makes post()/drain()'s bitset op atomic
// against a posting task. Held only across the bitset op, never the handler.
//
// Host build is single-threaded, so the guard is a no-op — which keeps
// DeferredActionQueue a pure, FreeRTOS-free, host-testable template.
#pragma once

namespace crosspoint {

#ifdef UNIT_TEST_HOST

struct DeferredActionGuard {
  DeferredActionGuard() {}
};

#else

}  // namespace crosspoint
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
namespace crosspoint {

struct DeferredActionGuard {
  DeferredActionGuard() { vTaskSuspendAll(); }
  ~DeferredActionGuard() { xTaskResumeAll(); }
  DeferredActionGuard(const DeferredActionGuard&) = delete;
  DeferredActionGuard& operator=(const DeferredActionGuard&) = delete;
};

#endif

}  // namespace crosspoint
