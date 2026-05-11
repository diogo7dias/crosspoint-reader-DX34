/**
 * @file PersistManager.h
 * @brief Coordinates multi-store tick/flush and first-boot SD sidecar backup.
 *
 * Backs persistAppState() in main.cpp: a single flushAll() call at
 * activity-transition chokepoints (RFC #20). Stores register themselves
 * via registerStore<T>().
 */
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "IAsyncRunner.h"
#include "IFileIO.h"

namespace crosspoint {
namespace persist {

// How a store wants its writes dispatched when callers invoke
// requestFlush(name). Sync runs flushNow on the caller's thread; Async
// hands flushNow to the persistence runner (FreeRTOS task on device,
// inline executor in host tests).
enum class WriteMode : uint8_t { Sync, Async };

class PersistManagerImpl {
 public:
  // Register a store — takes non-owning references to its tick/flush functions.
  struct Entry {
    std::function<bool(uint32_t)> tick;  // tickPersist(nowMs)
    std::function<bool()> flushNow;      // flushNow()
    std::function<bool()> isDirty;
    const char* name;
    const char* path;
    WriteMode mode = WriteMode::Sync;
  };

  void registerStore(Entry e) { stores_.push_back(std::move(e)); }

  // Honors the registered store's WriteMode. Sync: runs flushNow inline.
  // Async: submits flushNow to the persistence runner; returns immediately
  // (<1ms target on the page-turn hot path). Unknown name is a no-op.
  void requestFlush(const char* name);

  // Test seam: inject a synchronous IAsyncRunner so persistence logic can
  // be exercised without FreeRTOS. Pass nullptr to revert to the default
  // (lazy-binds to AsyncWriter::instance() on the next Async flush).
  // Must not be called concurrently with requestFlush.
  void setAsyncRunnerForTest(IAsyncRunner* runner) { asyncRunner_ = runner; }

  // Diagnostics passthrough. Returns 0 if no runner has been bound yet.
  size_t asyncDroppedCount() const {
    return asyncRunner_ ? asyncRunner_->droppedCount() : 0;
  }

  // Call from main loop. Ticks every store; returns count of flushes performed.
  size_t tick(uint32_t nowMs) {
    size_t flushed = 0;
    for (auto& s : stores_) {
      if (s.tick(nowMs)) ++flushed;
    }
    return flushed;
  }

  // Force sync flush of every dirty store. Used at activity transitions +
  // before deep sleep. Returns count of successful flushes.
  size_t flushAll() {
    size_t flushed = 0;
    for (auto& s : stores_) {
      if (!s.isDirty()) continue;
      if (s.flushNow()) ++flushed;
    }
    return flushed;
  }

  // First boot of a new firmware version: copy every registered store's
  // file to /.crosspoint/backup-<version>/<basename>. Idempotent — uses a
  // marker file to avoid re-backup on subsequent boots of the same version.
  // Caller passes a version identifier + the IFileIO to use.
  // Returns true if backup was attempted (new version detected).
  bool backupSidecarIfNewFirmware(IFileIO& io, const char* version);

  size_t storeCount() const { return stores_.size(); }
  void clearForTest() { stores_.clear(); }

 private:
  // Resolves a runner for an Async flush. On device builds, lazy-binds
  // to AsyncWriter::instance() on first use. On host builds without a
  // test seam set, returns nullptr (caller falls back to sync flush).
  IAsyncRunner* ensureRunner();

  std::vector<Entry> stores_;
  IAsyncRunner* asyncRunner_ = nullptr;  // non-owning; lazy-bound on device.
};

// Process-wide instance.
PersistManagerImpl& PersistManager();

}  // namespace persist
}  // namespace crosspoint
