#pragma once
#include <cstdint>

#include "FreeRTOS.h"

#ifndef pdPASS
#define pdPASS 1
#endif
#ifndef pdFAIL
#define pdFAIL 0
#endif
#ifndef tskNO_AFFINITY
#define tskNO_AFFINITY 0x7FFFFFFF
#endif

// Thread-local pointer so ulTaskNotifyTake can find the current task's handle.
inline thread_local SimTaskHandle* tl_currentTaskHandle = nullptr;

// Create a real OS thread. The FreeRTOS task function signature is void(*)(void*).
inline int xTaskCreate(void (*fn)(void*), const char* /*name*/, uint32_t /*stackDepth*/, void* param, int /*priority*/,
                       TaskHandle_t* handle) {
  auto* h = new SimTaskHandle();
  h->thread = std::thread([fn, param, h]() {
    tl_currentTaskHandle = h;
    h->id = std::this_thread::get_id();
    fn(param);
  });
  if (handle) *handle = h;
  return 1;  // pdPASS
}

// Block until notified (simulates ulTaskNotifyTake with clear-on-exit).
inline uint32_t ulTaskNotifyTake(int /*clearOnExit*/, uint32_t /*ticksToWait*/) {
  auto* h = tl_currentTaskHandle;
  if (!h) return 1;  // Not in a task thread, don't block
  std::unique_lock<std::mutex> lk(h->mtx);
  h->cv.wait(lk, [h] { return h->notifyCount > 0; });
  h->notifyCount--;
  return 1;
}

// Wake a task by incrementing its notification counter and signalling its condvar.
inline void xTaskNotify(TaskHandle_t handle, uint32_t /*value*/, int /*action*/) {
  if (!handle) return;
  {
    std::lock_guard<std::mutex> lk(handle->mtx);
    handle->notifyCount++;
  }
  handle->cv.notify_one();
}

inline TaskHandle_t xTaskGetCurrentTaskHandle() { return tl_currentTaskHandle; }

// Detach but DO NOT delete. The detached thread may still be inside
// `ulTaskNotifyTake` waiting on `h->cv` / `h->mtx`; destroying the handle
// would pull the rug out and libc++ raises EINVAL from the condvar wait.
// The running task detects exit via Activity::renderTaskHandle being set to
// null and returns from its while-loop, after which the thread ends cleanly.
// We intentionally leak `h` — ~200 bytes per activity switch, negligible
// for a dev simulator.
inline void vTaskDelete(TaskHandle_t h) {
  if (h && h->thread.joinable()) h->thread.detach();
}
inline unsigned int uxTaskGetStackHighWaterMark(TaskHandle_t) { return 2048; }
inline void vTaskList(char*) {}
inline void vTaskDelay(int) {}

// ESP32 dual-core variant — simulator is single-threaded, core arg ignored.
inline int xTaskCreatePinnedToCore(void (*fn)(void*), const char* name, uint32_t stackDepth, void* param,
                                    int priority, TaskHandle_t* handle, int /*coreId*/) {
  return xTaskCreate(fn, name, stackDepth, param, priority, handle);
}
