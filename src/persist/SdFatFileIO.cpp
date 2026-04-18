#include "SdFatFileIO.h"

#ifndef UNIT_TEST_HOST

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

namespace crosspoint {
namespace persist {

namespace {

constexpr size_t kMaxPathLen = 119;  // same bound as JsonSettingsIO::safeWriteFile

bool ensureParentDirectory(const char* targetPath) {
  const char* slash = strrchr(targetPath, '/');
  if (!slash || slash == targetPath) {
    return true;
  }

  char parentPath[128];
  const size_t parentLen = static_cast<size_t>(slash - targetPath);
  if (parentLen >= sizeof(parentPath)) {
    LOG_ERR("PST", "ensureParent: path too long for %s", targetPath);
    return false;
  }
  memcpy(parentPath, targetPath, parentLen);
  parentPath[parentLen] = '\0';

  if (Storage.exists(parentPath)) {
    FsFile entry = Storage.open(parentPath, O_RDONLY);
    if (entry) {
      const bool isDirectory = entry.isDirectory();
      entry.close();
      if (isDirectory) return true;
    }
    // Non-dir blocking creation — quarantine.
    char quarantinePath[160];
    snprintf(quarantinePath, sizeof(quarantinePath), "%s.corrupt", parentPath);
    if (Storage.exists(quarantinePath)) {
      if (!Storage.remove(quarantinePath)) Storage.removeDir(quarantinePath);
    }
    if (!Storage.rename(parentPath, quarantinePath)) {
      if (!Storage.remove(parentPath)) {
        LOG_ERR("PST", "ensureParent: cannot quarantine %s", parentPath);
        return false;
      }
    } else {
      LOG_ERR("PST", "ensureParent: quarantined invalid parent %s", parentPath);
    }
  }

  if (!Storage.mkdir(parentPath)) {
    FsFile dir = Storage.open(parentPath, O_RDONLY);
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      LOG_ERR("PST", "ensureParent: failed to create %s", parentPath);
      return false;
    }
    dir.close();
  }
  return true;
}

}  // namespace

bool SdFatFileIO::safeWrite(const std::string& path, const std::string& content) {
  if (path.empty() || path.size() > kMaxPathLen) {
    LOG_ERR("PST", "safeWrite: path null or too long");
    return false;
  }
  if (!ensureParentDirectory(path.c_str())) return false;

  char tmpPath[128];
  char bakPath[128];
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path.c_str());
  snprintf(bakPath, sizeof(bakPath), "%s.bak", path.c_str());

  // Stuck-.tmp fallback (MEMORY.md bug fix).
  const char* activeTmp = tmpPath;
  char altTmpPath[128];
  if (Storage.exists(tmpPath)) {
    if (!Storage.remove(tmpPath)) {
      LOG_ERR("PST", "safeWrite: stale tmp %s stuck; using .tmp2", tmpPath);
      snprintf(altTmpPath, sizeof(altTmpPath), "%s.tmp2", path.c_str());
      activeTmp = altTmpPath;
    }
  }

  // HalStorage.writeFile takes Arduino String. std::string → String round-trip.
  String payload;
  payload.reserve(content.size());
  payload.concat(content.c_str(), content.size());
  if (!Storage.writeFile(activeTmp, payload)) {
    LOG_ERR("PST", "safeWrite: failed to write tmp %s", activeTmp);
    return false;
  }

  if (Storage.exists(bakPath)) {
    if (!Storage.remove(bakPath)) {
      LOG_ERR("PST", "safeWrite: failed to remove stale bak %s", bakPath);
    }
  }
  if (Storage.exists(path.c_str())) {
    if (!Storage.rename(path.c_str(), bakPath)) {
      LOG_ERR("PST", "safeWrite: failed to rotate %s → %s", path.c_str(), bakPath);
      Storage.remove(activeTmp);
      return false;
    }
  }
  if (!Storage.rename(activeTmp, path.c_str())) {
    LOG_ERR("PST", "safeWrite: failed to promote %s", activeTmp);
    if (Storage.exists(bakPath)) {
      if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
      if (Storage.rename(bakPath, path.c_str())) {
        LOG_ERR("PST", "safeWrite: restored %s from bak", path.c_str());
      } else {
        LOG_ERR("PST", "safeWrite: cannot restore %s from bak", path.c_str());
      }
    }
    return false;
  }
  return true;
}

std::string SdFatFileIO::safeRead(const std::string& path) {
  auto toStd = [](const String& s) -> std::string { return std::string(s.c_str(), s.length()); };
  if (Storage.exists(path.c_str())) {
    String json = Storage.readFile(path.c_str());
    if (!json.isEmpty()) return toStd(json);
  }
  char bakPath[128];
  snprintf(bakPath, sizeof(bakPath), "%s.bak", path.c_str());
  if (Storage.exists(bakPath)) {
    String json = Storage.readFile(bakPath);
    if (!json.isEmpty()) {
      LOG_DBG("PST", "safeRead: recovered %s from .bak", path.c_str());
      return toStd(json);
    }
  }
  char tmpPath[128];
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path.c_str());
  if (Storage.exists(tmpPath)) {
    String json = Storage.readFile(tmpPath);
    if (!json.isEmpty()) {
      LOG_DBG("PST", "safeRead: recovered %s from .tmp", path.c_str());
      return toStd(json);
    }
  }
  return {};
}

bool SdFatFileIO::exists(const std::string& path) { return Storage.exists(path.c_str()); }

bool SdFatFileIO::mkdir(const std::string& path) { return Storage.mkdir(path.c_str()); }

bool SdFatFileIO::copy(const std::string& from, const std::string& to) {
  if (!Storage.exists(from.c_str())) return false;
  String content = Storage.readFile(from.c_str());
  return Storage.writeFile(to.c_str(), content);
}

}  // namespace persist
}  // namespace crosspoint

#endif  // !UNIT_TEST_HOST
