#include "PersistManager.h"

#include <cstring>
#include <string>

#ifndef UNIT_TEST_HOST
#include "AsyncWriter.h"
#endif

namespace crosspoint {
namespace persist {

namespace {

const char* kBackupRoot = "/.crosspoint/backups";

std::string basename(const char* path) {
  const char* slash = strrchr(path, '/');
  return std::string(slash ? slash + 1 : path);
}

}  // namespace

PersistManagerImpl& PersistManager() {
  static PersistManagerImpl inst;
  return inst;
}

IAsyncRunner* PersistManagerImpl::ensureRunner() {
  if (asyncRunner_ != nullptr) return asyncRunner_;
#ifndef UNIT_TEST_HOST
  // Default device wiring: lazy-bind to the singleton FreeRTOS-backed
  // runner. start() is idempotent so this is safe even if main.cpp has
  // already brought it online at boot.
  asyncRunner_ = &AsyncWriter::instance();
  asyncRunner_->start();
#endif
  return asyncRunner_;
}

void PersistManagerImpl::requestFlush(const char* name) {
  if (!name) return;
  for (auto& s : stores_) {
    if (strcmp(s.name, name) != 0) continue;
    if (s.mode == WriteMode::Sync) {
      if (s.flushNow) s.flushNow();
      return;
    }
    // Async mode.
    IAsyncRunner* runner = ensureRunner();
    if (runner == nullptr) {
      // No runner bound (host build without test seam). Best-effort
      // fallback: run synchronously so data is not lost.
      if (s.flushNow) s.flushNow();
      return;
    }
    auto fn = s.flushNow;  // copy std::function — runs on the runner.
    runner->submit([fn]() {
      if (fn) fn();
    });
    return;
  }
}

bool PersistManagerImpl::backupSidecarIfNewFirmware(IFileIO& io, const char* version) {
  if (!version || !*version) return false;

  // Marker path: /.crosspoint/backups/.marker-<version>
  std::string markerPath = std::string(kBackupRoot) + "/.marker-" + version;
  if (io.exists(markerPath)) {
    return false;  // already backed up for this version
  }

  // Backup dir for this version.
  std::string backupDir = std::string(kBackupRoot) + "/" + version;
  io.mkdir(kBackupRoot);
  io.mkdir(backupDir);

  // Copy each registered store's file into the backup dir.
  for (const auto& s : stores_) {
    if (!io.exists(s.path)) continue;
    const std::string target = backupDir + "/" + basename(s.path);
    io.copy(s.path, target);
  }

  // Drop the marker so we don't repeat on the next boot.
  io.safeWrite(markerPath, std::string(version));
  return true;
}

}  // namespace persist
}  // namespace crosspoint
