/**
 * @file PersistManager.h
 * @brief Coordinates multi-store tick/flush and first-boot SD sidecar backup.
 *
 * Under PERSIST_V2 this replaces persistAppState() in main.cpp with a
 * single flushAll() call at activity-transition chokepoints. Stores
 * register themselves via registerStore<T>().
 */
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "IFileIO.h"

namespace crosspoint {
namespace persist {

class PersistManagerImpl {
 public:
  // Register a store — takes non-owning references to its tick/flush functions.
  struct Entry {
    std::function<bool(uint32_t)> tick;  // tickPersist(nowMs)
    std::function<bool()> flushNow;      // flushNow()
    std::function<bool()> isDirty;
    const char* name;
    const char* path;
  };

  void registerStore(Entry e) { stores_.push_back(std::move(e)); }

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
  std::vector<Entry> stores_;
};

// Process-wide instance.
PersistManagerImpl& PersistManager();

}  // namespace persist
}  // namespace crosspoint
