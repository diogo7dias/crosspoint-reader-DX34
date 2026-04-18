#include "SdFatSleepFs.h"

#include <HalStorage.h>

#include <algorithm>
#include <cstring>

namespace crosspoint {
namespace sleep {
namespace {

constexpr const char* kSleepDir = "/sleep";

bool isBmpName(const char* name) {
  if (!name || name[0] == '\0' || name[0] == '.') return false;
  const size_t len = std::strlen(name);
  if (len < 4) return false;
  return std::strcmp(name + len - 4, ".bmp") == 0;
}

}  // namespace

size_t SdFatSleepFs::countSleepBmps(size_t scanCap) {
  auto dir = Storage.open(kSleepDir);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }
  size_t count = 0;
  char name[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      if (isBmpName(name)) {
        ++count;
        if (count > scanCap) {
          file.close();
          break;
        }
      }
    }
    file.close();
  }
  dir.close();
  return count;
}

std::vector<std::string> SdFatSleepFs::listSleepBmps(size_t maxEntries) {
  std::vector<std::string> out;
  if (maxEntries == 0) return out;
  auto dir = Storage.open(kSleepDir);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return out;
  }
  out.reserve(std::min<size_t>(maxEntries, 256));
  char name[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      if (isBmpName(name)) {
        out.emplace_back(name);
        if (out.size() >= maxEntries) {
          file.close();
          break;
        }
      }
    }
    file.close();
  }
  dir.close();
  std::sort(out.begin(), out.end());
  return out;
}

std::string SdFatSleepFs::nextSleepBmpAfter(const std::string& after) {
  auto dir = Storage.open(kSleepDir);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return "";
  }
  std::string minName;     // lex-min (fallback for wrap)
  std::string nextName;    // lex-smallest strictly greater than `after`
  char name[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      if (isBmpName(name)) {
        const std::string candidate(name);
        if (minName.empty() || candidate < minName) minName = candidate;
        if (!after.empty() && candidate > after) {
          if (nextName.empty() || candidate < nextName) nextName = candidate;
        }
      }
    }
    file.close();
  }
  dir.close();
  if (!after.empty() && !nextName.empty()) return nextName;
  return minName;
}

std::string SdFatSleepFs::nthSleepBmp(size_t n) {
  auto dir = Storage.open(kSleepDir);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return "";
  }
  size_t idx = 0;
  std::string result;
  char name[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      if (isBmpName(name)) {
        if (idx == n) {
          result = name;
          file.close();
          break;
        }
        ++idx;
      }
    }
    file.close();
  }
  dir.close();
  return result;
}

bool SdFatSleepFs::exists(const std::string& path) { return Storage.exists(path.c_str()); }

bool SdFatSleepFs::mkdir(const std::string& path) { return Storage.mkdir(path.c_str()); }

bool SdFatSleepFs::rename(const std::string& from, const std::string& to) {
  return Storage.rename(from.c_str(), to.c_str());
}

}  // namespace sleep
}  // namespace crosspoint
