#include "AsyncWriter.h"

#include <Arduino.h>
#include <Logging.h>

namespace crosspoint {
namespace persist {

AsyncWriter& AsyncWriter::instance() {
  static AsyncWriter s;
  return s;
}

void AsyncWriter::start() {
  if (task_ != nullptr) {
    return;
  }
  if (mtx_ == nullptr) {
    mtx_ = xSemaphoreCreateMutex();
  }
  // Stack 4 KB: holds std::function captures + SD ops. HalStorage already
  // serializes SdFat via its own mutex, so the SD write call chain is safe
  // here. Priority 1 = same as main loopTask; FreeRTOS preemptive scheduling
  // (configUSE_TIME_SLICING) splits the single CPU between us so neither
  // task blocks the other for the full 120 ms write.
  const BaseType_t ok = xTaskCreate(&AsyncWriter::taskEntry, "asyncwriter", 4096, this, 1, &task_);
  if (ok != pdPASS) {
    LOG_ERR("AWR", "xTaskCreate failed");
    task_ = nullptr;
    // Release the mutex too so a later start() retry begins from a clean
    // slate (otherwise we leave the semaphore handle pinned to this
    // singleton with no task to take it).
    if (mtx_ != nullptr) {
      vSemaphoreDelete(mtx_);
      mtx_ = nullptr;
    }
  }
}

void AsyncWriter::submit(std::function<void()> fn) {
  if (mtx_ == nullptr || task_ == nullptr) {
    // Not started — execute synchronously as a degraded fallback so we never
    // silently drop writes during boot before start() is called.
    if (fn) fn();
    return;
  }
  xSemaphoreTake(mtx_, portMAX_DELAY);
  if (count_ == kMaxQueue) {
    // Drop oldest. Latest snapshot is what matters; older positions are stale.
    queue_[head_] = std::function<void()>();
    head_ = (head_ + 1) % kMaxQueue;
    --count_;
    ++dropped_;
  }
  queue_[tail_] = std::move(fn);
  tail_ = (tail_ + 1) % kMaxQueue;
  ++count_;
  xSemaphoreGive(mtx_);
  xTaskNotifyGive(task_);
}

void AsyncWriter::drainBlocking() {
  if (mtx_ == nullptr) {
    return;
  }
  while (true) {
    xSemaphoreTake(mtx_, portMAX_DELAY);
    const bool done = (count_ == 0) && !busy_;
    xSemaphoreGive(mtx_);
    if (done) {
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void AsyncWriter::taskEntry(void* arg) { static_cast<AsyncWriter*>(arg)->taskLoop(); }

void AsyncWriter::taskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (true) {
      std::function<void()> job;
      xSemaphoreTake(mtx_, portMAX_DELAY);
      if (count_ == 0) {
        busy_ = false;
        xSemaphoreGive(mtx_);
        break;
      }
      job = std::move(queue_[head_]);
      queue_[head_] = std::function<void()>();
      head_ = (head_ + 1) % kMaxQueue;
      --count_;
      busy_ = true;
      xSemaphoreGive(mtx_);

      if (job) {
        job();
      }

      xSemaphoreTake(mtx_, portMAX_DELAY);
      busy_ = false;
      xSemaphoreGive(mtx_);
    }
  }
}

}  // namespace persist
}  // namespace crosspoint
