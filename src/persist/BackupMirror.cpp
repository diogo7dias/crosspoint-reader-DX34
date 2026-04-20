#include "BackupMirror.h"

#include <HalStorage.h>
#include <Logging.h>
#include <esp_task_wdt.h>

#include "Paths.h"

namespace backup {
namespace {

constexpr const char* kLog = "BMR";

bool copyFile(const std::string& srcPath, const std::string& dstPath) {
  if (!Storage.exists(srcPath.c_str())) return false;

  HalFile src;
  if (!Storage.openFileForRead(kLog, srcPath, src)) return false;

  HalFile dst;
  if (!Storage.openFileForWrite(kLog, dstPath, dst)) {
    src.close();
    return false;
  }

  uint8_t buffer[512];
  while (src.available()) {
    const int rd = src.read(buffer, sizeof(buffer));
    if (rd <= 0) break;
    if (dst.write(buffer, rd) != static_cast<size_t>(rd)) {
      src.close();
      dst.close();
      Storage.remove(dstPath.c_str());
      return false;
    }
  }
  dst.flush();
  dst.close();
  src.close();
  return true;
}

std::string basenameOf(const std::string& path) {
  const auto slash = path.rfind('/');
  return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

// Lowercase; keep [a-z0-9._-]; space -> _; everything else dropped. Cap 80.
std::string sanitize(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    const auto uc = static_cast<unsigned char>(c);
    if (uc >= 'A' && uc <= 'Z') {
      out.push_back(static_cast<char>(uc - 'A' + 'a'));
    } else if ((uc >= 'a' && uc <= 'z') || (uc >= '0' && uc <= '9') || uc == '.' || uc == '_' || uc == '-') {
      out.push_back(static_cast<char>(uc));
    } else if (uc == ' ') {
      out.push_back('_');
    }
    if (out.size() >= 80) break;
  }
  return out;
}

bool startsWith(const std::string& s, const char* prefix) {
  const size_t n = strlen(prefix);
  return s.size() >= n && s.compare(0, n, prefix) == 0;
}

bool endsWith(const std::string& s, const char* suffix) {
  const size_t n = strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

void snapshotBookCaches() {
  HalFile root = Storage.open(Paths::kDataDir, O_RDONLY);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  for (HalFile entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    esp_task_wdt_reset();
    if (!entry.isDirectory()) {
      entry.close();
      continue;
    }
    char nameBuf[96];
    entry.getName(nameBuf, sizeof(nameBuf));
    entry.close();
    const std::string dirName = nameBuf;
    if (!startsWith(dirName, "epub_") && !startsWith(dirName, "xtc_") && !startsWith(dirName, "txt_")) {
      continue;
    }
    const std::string cachePath = std::string(Paths::kDataDir) + "/" + dirName;
    const char* files[] = {"progress.bin", "bookmarks.json", "reader_settings.json"};
    for (const char* f : files) {
      const std::string src = cachePath + "/" + f;
      if (!Storage.exists(src.c_str())) continue;
      const std::string dst = std::string(Paths::kBackupDir) + "/" + dirName + "_" + f;
      if (!copyFile(src, dst)) {
        LOG_ERR(kLog, "mirror copy failed %s -> %s", src.c_str(), dst.c_str());
      }
    }
  }
  root.close();
}

void snapshotQuotesSidecars() {
  HalFile recents = Storage.open("/recents", O_RDONLY);
  if (!recents || !recents.isDirectory()) {
    if (recents) recents.close();
    return;
  }

  for (HalFile entry = recents.openNextFile(); entry; entry = recents.openNextFile()) {
    esp_task_wdt_reset();
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }
    char nameBuf[160];
    entry.getName(nameBuf, sizeof(nameBuf));
    entry.close();
    const std::string filename = nameBuf;
    if (!endsWith(filename, "_QUOTES.txt")) continue;
    const std::string src = std::string("/recents/") + filename;
    const std::string flat = "quotes_" + sanitize(filename);
    const std::string dst = std::string(Paths::kBackupDir) + "/" + flat;
    if (!copyFile(src, dst)) {
      LOG_ERR(kLog, "mirror copy failed %s", src.c_str());
    }
  }
  recents.close();
}

void snapshotGlobal() {
  struct Pair {
    const char* src;
    const char* flat;
  };
  const Pair files[] = {
      {"/.crosspoint/state.json", "global_state.json"},
      {"/.crosspoint/settings.json", "global_settings.json"},
      {"/.crosspoint/wifi.json", "global_wifi.json"},
      {"/.crosspoint/reading_themes.json", "global_reading_themes.json"},
      {"/.crosspoint/recent.json", "global_recent.json"},
  };
  for (const auto& p : files) {
    if (!Storage.exists(p.src)) continue;
    const std::string dst = std::string(Paths::kBackupDir) + "/" + p.flat;
    if (!copyFile(p.src, dst)) {
      LOG_ERR(kLog, "mirror copy failed %s", p.src);
    }
  }
}

}  // namespace

void snapshotAll() {
  Storage.mkdir(Paths::kBackupDir);
  snapshotBookCaches();
  snapshotQuotesSidecars();
  snapshotGlobal();
  LOG_DBG(kLog, "snapshotAll complete");
}

std::string flatNameForCacheFile(const std::string& cachePath, const std::string& logicalFile) {
  return basenameOf(cachePath) + "_" + logicalFile;
}

std::string flatNameForQuotesPath(const std::string& quotesPath) {
  return "quotes_" + sanitize(basenameOf(quotesPath));
}

bool restoreFromMirror(const std::string& flatName, const std::string& destPath) {
  const std::string src = std::string(Paths::kBackupDir) + "/" + flatName;
  if (!Storage.exists(src.c_str())) return false;
  return copyFile(src, destPath);
}

}  // namespace backup
