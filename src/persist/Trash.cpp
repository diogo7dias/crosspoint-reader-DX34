#include "Trash.h"

#include <BookFingerprint.h>
#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Paths.h"
#include "util/StringUtils.h"

namespace trash {
namespace {

constexpr const char* kLog = "TRH";

bool endsWithIgnoreCase(const std::string& s, const char* suffix) {
  const size_t n = strlen(suffix);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; i++) {
    const char a = s[s.size() - n + i];
    const char b = suffix[i];
    const char la = (a >= 'A' && a <= 'Z') ? static_cast<char>(a - 'A' + 'a') : a;
    const char lb = (b >= 'A' && b <= 'Z') ? static_cast<char>(b - 'A' + 'a') : b;
    if (la != lb) return false;
  }
  return true;
}

bool isBookPath(const std::string& path) {
  return endsWithIgnoreCase(path, ".epub") || endsWithIgnoreCase(path, ".xtch") ||
         endsWithIgnoreCase(path, ".xtc") || endsWithIgnoreCase(path, ".txt") || endsWithIgnoreCase(path, ".md");
}

std::string basenameOf(const std::string& path) {
  const auto slash = path.rfind('/');
  return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

// Parse leading zero-padded number from slot name "NNNN_rest".
// Returns -1 if no leading number.
int leadingNumber(const std::string& slotName) {
  size_t i = 0;
  while (i < slotName.size() && slotName[i] >= '0' && slotName[i] <= '9') i++;
  if (i == 0) return -1;
  return std::atoi(slotName.substr(0, i).c_str());
}

// Scan /.crosspoint/trash/ and return sorted slot names (ascending by number).
// Non-numeric entries are ignored.
std::vector<std::string> listSlotsSortedAsc() {
  std::vector<std::string> slots;
  HalFile dir = Storage.open(Paths::kTrashDir, O_RDONLY);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return slots;
  }
  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (entry.isDirectory()) {
      char nameBuf[128];
      entry.getName(nameBuf, sizeof(nameBuf));
      const std::string n = nameBuf;
      if (leadingNumber(n) >= 0) {
        slots.push_back(n);
      }
    }
    entry.close();
  }
  dir.close();
  std::sort(slots.begin(), slots.end(), [](const std::string& a, const std::string& b) {
    return leadingNumber(a) < leadingNumber(b);
  });
  return slots;
}

int nextSequenceNumber() {
  const auto slots = listSlotsSortedAsc();
  if (slots.empty()) return 1;
  return leadingNumber(slots.back()) + 1;
}

std::string cachePathForBook(const std::string& path) {
  if (endsWithIgnoreCase(path, ".epub")) {
    return Epub(path, Paths::kDataDir).getCachePath();
  }
  if (endsWithIgnoreCase(path, ".xtc") || endsWithIgnoreCase(path, ".xtch")) {
    return Xtc(path, Paths::kDataDir).getCachePath();
  }
  if (endsWithIgnoreCase(path, ".txt") || endsWithIgnoreCase(path, ".md")) {
    return Txt(path, Paths::kDataDir).getCachePath();
  }
  return {};
}

std::string quotesSidecarForBook(const std::string& path) {
  const auto dot = path.rfind('.');
  const std::string base = (dot != std::string::npos) ? path.substr(0, dot) : path;
  return base + "_QUOTES.txt";
}

}  // namespace

bool moveToTrash(const std::string& path) {
  if (path.empty() || !Storage.exists(path.c_str())) return false;

  Storage.mkdir(Paths::kTrashDir);

  char seqBuf[8];
  const int seq = nextSequenceNumber();
  std::snprintf(seqBuf, sizeof(seqBuf), "%04d", seq);

  const std::string basename = basenameOf(path);
  const std::string slotName = std::string(seqBuf) + "_" + basename;
  const std::string slotPath = std::string(Paths::kTrashDir) + "/" + slotName;

  if (!Storage.mkdir(slotPath.c_str())) {
    LOG_ERR(kLog, "Failed to create trash slot %s", slotPath.c_str());
    return false;
  }

  const bool isBook = isBookPath(path);

  // Move book cache dir into slot/cache (before moving the book itself, so
  // fingerprint lookup against the book still works).
  if (isBook) {
    const std::string cachePath = cachePathForBook(path);
    if (!cachePath.empty() && Storage.exists(cachePath.c_str())) {
      const std::string dest = slotPath + "/cache";
      if (!Storage.rename(cachePath.c_str(), dest.c_str())) {
        LOG_ERR(kLog, "Failed to move cache to trash: %s -> %s", cachePath.c_str(), dest.c_str());
      }
    }
    esp_task_wdt_reset();

    const std::string quotesPath = quotesSidecarForBook(path);
    if (Storage.exists(quotesPath.c_str())) {
      const std::string dest = slotPath + "/" + basenameOf(quotesPath);
      if (!Storage.rename(quotesPath.c_str(), dest.c_str())) {
        LOG_ERR(kLog, "Failed to move quotes sidecar to trash: %s", quotesPath.c_str());
      }
    }
  }

  // Move the primary file into the slot.
  const std::string fileDest = slotPath + "/" + basename;
  if (!Storage.rename(path.c_str(), fileDest.c_str())) {
    LOG_ERR(kLog, "Failed to move %s into trash slot %s", path.c_str(), slotPath.c_str());
    return false;
  }

  LOG_INF(kLog, "Moved %s to trash slot %s", path.c_str(), slotPath.c_str());
  return true;
}

void pruneToCap(size_t cap) {
  const auto slots = listSlotsSortedAsc();
  if (slots.size() <= cap) return;

  const size_t toRemove = slots.size() - cap;
  for (size_t i = 0; i < toRemove; i++) {
    esp_task_wdt_reset();
    const std::string full = std::string(Paths::kTrashDir) + "/" + slots[i];
    if (Storage.removeDir(full.c_str())) {
      LOG_INF(kLog, "Pruned trash slot %s", slots[i].c_str());
    } else {
      LOG_ERR(kLog, "Failed to prune trash slot %s", full.c_str());
    }
  }
}

}  // namespace trash
