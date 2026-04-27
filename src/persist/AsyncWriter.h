/**
 * @file AsyncWriter.h
 * @brief Background SD-write task that unblocks the main loop.
 *
 * Singleton FreeRTOS task that drains submitted callbacks. Lets per-page-turn
 * progress saves return immediately from the main loop; the actual ~120 ms
 * SD atomic-rename happens on a background task that time-shares the single
 * ESP32-C3 CPU with the main loopTask via FreeRTOS preemptive scheduling.
 *
 * Lifecycle paths (onExit, enterDeepSleep) MUST call drainBlocking() before
 * powering down to guarantee queued writes hit SD.
 */
#pragma once

#include <cstddef>
#include <functional>

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
}

namespace crosspoint {
namespace persist {

class AsyncWriter {
 public:
  static AsyncWriter& instance();

  // Idempotent. Creates the task on first call. Safe to call from setup().
  void start();

  // Enqueue a callback. Returns immediately. If queue at capacity, the
  // OLDEST pending job is dropped (latest write always wins; older snapshots
  // are stale anyway).
  void submit(std::function<void()> fn);

  // Block caller until queue empty AND any in-flight job has completed.
  // Use at lifecycle boundaries before powering down or destroying captured
  // state. Polls every 1 ms; cheap on lifecycle paths.
  void drainBlocking();

  size_t droppedCount() const { return dropped_; }

 private:
  AsyncWriter() = default;
  AsyncWriter(const AsyncWriter&) = delete;
  AsyncWriter& operator=(const AsyncWriter&) = delete;

  static void taskEntry(void* arg);
  void taskLoop();

  TaskHandle_t task_ = nullptr;
  SemaphoreHandle_t mtx_ = nullptr;
  // Small ring buffer (size = kMaxQueue + 1). Older entries dropped at cap.
  // std::function lives on heap; queue itself is fixed size to bound memory.
  static constexpr size_t kMaxQueue = 4;
  std::function<void()> queue_[kMaxQueue];
  size_t head_ = 0;  // next pop
  size_t tail_ = 0;  // next push
  size_t count_ = 0;
  size_t dropped_ = 0;
  bool busy_ = false;
};

}  // namespace persist
}  // namespace crosspoint
