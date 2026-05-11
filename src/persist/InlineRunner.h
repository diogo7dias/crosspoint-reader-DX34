/**
 * @file InlineRunner.h
 * @brief Synchronous IAsyncRunner used by host tests.
 *
 * Executes every submitted callable immediately on the caller's thread.
 * Queue depth is effectively unbounded (we never queue — we run).
 * Drop counter is always zero. drainBlocking is a no-op because nothing
 * is ever in flight after submit() returns.
 *
 * Production code MUST NOT depend on this class. It exists solely so
 * persistence logic that uses the IAsyncRunner port can be exercised in
 * unit tests without FreeRTOS, mutexes, or task notifications.
 */
#pragma once

#include <cstddef>
#include <functional>
#include <utility>

#include "IAsyncRunner.h"

namespace crosspoint {
namespace persist {

class InlineRunner : public IAsyncRunner {
 public:
  void start() override { started_ = true; }

  void submit(std::function<void()> fn) override {
    ++submitted_;
    if (fn) fn();
  }

  void drainBlocking() override {
    // Nothing in flight: submit ran synchronously.
  }

  std::size_t droppedCount() const override { return 0; }

  // Test-only introspection. Not part of the IAsyncRunner contract.
  bool started() const { return started_; }
  std::size_t submittedCount() const { return submitted_; }
  void resetForTest() {
    started_ = false;
    submitted_ = 0;
  }

 private:
  bool started_ = false;
  std::size_t submitted_ = 0;
};

}  // namespace persist
}  // namespace crosspoint
