/**
 * @file IAsyncRunner.h
 * @brief Port for the background-write dispatcher used by PersistManager.
 *
 * Today's only production implementation wraps a FreeRTOS task (see
 * AsyncWriter). Host tests can substitute an inline runner that executes
 * submitted callables synchronously on the caller's thread, so persistence
 * logic can be exercised without FreeRTOS.
 *
 * This interface is intentionally tiny — it mirrors the surface AsyncWriter
 * already presents. Future RFC steps move ownership of the runner inside
 * PersistManager and shrink the caller-visible surface further.
 */
#pragma once

#include <cstddef>
#include <functional>

namespace crosspoint {
namespace persist {

class IAsyncRunner {
 public:
  virtual ~IAsyncRunner() = default;

  // Idempotent. Brings the runner online (creates the task, allocates the
  // mutex, etc.). Safe to call from setup().
  virtual void start() = 0;

  // Enqueue a callable. Returns immediately. If the runner's queue is at
  // capacity, the implementation MAY drop the oldest pending job; callers
  // must treat submission as best-effort and rely on the drop counter or
  // lifecycle-flush re-issue to recover.
  virtual void submit(std::function<void()> fn) = 0;

  // Block until the queue is empty AND any in-flight job has completed.
  // Used at lifecycle boundaries (deep sleep, activity exit) before
  // destroying captured state.
  virtual void drainBlocking() = 0;

  // Monotonically increasing count of jobs dropped due to queue saturation.
  // Diagnostics only; the runner also logs each drop.
  virtual std::size_t droppedCount() const = 0;
};

}  // namespace persist
}  // namespace crosspoint
