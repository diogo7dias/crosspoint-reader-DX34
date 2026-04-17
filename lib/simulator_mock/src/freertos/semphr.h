#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "FreeRTOS.h"

// FreeRTOS has two flavors we care about:
//   1. Recursive mutex (xSemaphoreCreateMutex) — owned by one thread, recursive takes OK.
//   2. Binary semaphore (xSemaphoreCreateBinary) — signal passed cross-thread; Take and Give
//      can happen on different threads.
//
// A single std::recursive_mutex cannot model (2): std::mutex::unlock() from a non-owning
// thread is UB and throws system_error on libc++. So SimSemaphore carries both a recursive
// mutex and a binary-counter+condvar; the boolean `isBinary` selects which primitive the
// Take/Give calls touch.

struct SimSemaphore {
  bool isBinary = false;

  // Recursive-mutex flavor
  std::recursive_mutex mtx;
  TaskHandle_t holder = nullptr;

  // Binary flavor (condvar-backed counter)
  std::mutex binMtx;
  std::condition_variable binCv;
  int count = 0;
};
typedef SimSemaphore* SemaphoreHandle_t;

inline SemaphoreHandle_t xSemaphoreCreateMutex() {
  auto* s = new SimSemaphore();
  s->isBinary = false;
  return s;
}

inline SemaphoreHandle_t xSemaphoreCreateBinary() {
  auto* s = new SimSemaphore();
  s->isBinary = true;
  s->count = 0;  // Binary sem starts empty — matches FreeRTOS behavior
  return s;
}

inline void vSemaphoreDelete(SemaphoreHandle_t sem) { delete sem; }

inline bool xSemaphoreTake(SemaphoreHandle_t sem, uint32_t ticksToWait) {
  if (!sem) return true;
  if (sem->isBinary) {
    std::unique_lock<std::mutex> lk(sem->binMtx);
    if (ticksToWait == 0) {
      if (sem->count <= 0) return false;  // pdFALSE — non-blocking miss
      sem->count--;
      return true;
    }
    // portMAX_DELAY → wait forever; otherwise bounded wait in ms (tick==1ms).
    if (ticksToWait == portMAX_DELAY) {
      sem->binCv.wait(lk, [sem] { return sem->count > 0; });
    } else {
      if (!sem->binCv.wait_for(lk, std::chrono::milliseconds(ticksToWait),
                                [sem] { return sem->count > 0; })) {
        return false;  // Timeout
      }
    }
    sem->count--;
    return true;
  }
  sem->mtx.lock();
  return true;
}

inline bool xSemaphoreGive(SemaphoreHandle_t sem) {
  if (!sem) return true;
  if (sem->isBinary) {
    std::lock_guard<std::mutex> lk(sem->binMtx);
    sem->count = 1;  // Binary: saturates at 1
    sem->binCv.notify_one();
    return true;
  }
  sem->holder = nullptr;
  sem->mtx.unlock();
  return true;
}

inline TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t sem) { return sem ? sem->holder : nullptr; }

// xQueuePeek on a mutex: returns pdTRUE if the mutex is available (not taken).
inline int xQueuePeek(SemaphoreHandle_t sem, void*, uint32_t) {
  if (!sem) return pdTRUE;
  if (sem->isBinary) return sem->count > 0 ? pdTRUE : pdFALSE;
  bool locked = sem->mtx.try_lock();
  if (locked) {
    sem->mtx.unlock();
    return pdTRUE;
  }
  return pdFALSE;
}
