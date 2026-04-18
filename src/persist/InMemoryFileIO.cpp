#include "InMemoryFileIO.h"

namespace crosspoint {
namespace persist {

bool InMemoryFileIO::safeWrite(const std::string& path, const std::string& content) {
  if (path.empty()) return false;
  const std::string tmp = path + ".tmp";
  const std::string bak = path + ".bak";

  // Step 1: write .tmp (wiping any stale .tmp first).
  files_.erase(tmp);
  if (failStep_ == 1) {
    failStep_ = 0;
    return false;
  }
  files_[tmp] = content;

  // Step 2: remove stale .bak.
  if (failStep_ == 2) {
    failStep_ = 0;
    return false;
  }
  files_.erase(bak);

  // Step 3: rotate real → .bak.
  if (failStep_ == 3) {
    failStep_ = 0;
    return false;
  }
  auto it = files_.find(path);
  if (it != files_.end()) {
    files_[bak] = it->second;
    files_.erase(it);
  }

  // Step 4: promote .tmp → real.
  if (failStep_ == 4) {
    failStep_ = 0;
    // Leave .tmp in place; caller should attempt rollback from .bak on next read.
    return false;
  }
  files_[path] = files_[tmp];
  files_.erase(tmp);
  ++writeCount_;
  return true;
}

std::string InMemoryFileIO::safeRead(const std::string& path) {
  auto it = files_.find(path);
  if (it != files_.end() && !it->second.empty()) return it->second;
  it = files_.find(path + ".bak");
  if (it != files_.end() && !it->second.empty()) return it->second;
  it = files_.find(path + ".tmp");
  if (it != files_.end() && !it->second.empty()) return it->second;
  return {};
}

bool InMemoryFileIO::exists(const std::string& path) { return files_.count(path) > 0; }

bool InMemoryFileIO::mkdir(const std::string& /*path*/) { return true; }

bool InMemoryFileIO::copy(const std::string& from, const std::string& to) {
  auto it = files_.find(from);
  if (it == files_.end()) return false;
  files_[to] = it->second;
  return true;
}

void InMemoryFileIO::put(const std::string& path, const std::string& content) { files_[path] = content; }

void InMemoryFileIO::erase(const std::string& path) { files_.erase(path); }

bool InMemoryFileIO::has(const std::string& path) const { return files_.count(path) > 0; }

}  // namespace persist
}  // namespace crosspoint
