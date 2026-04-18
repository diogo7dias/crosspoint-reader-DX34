#include "PersistManager.h"

#include <cstring>
#include <string>

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
