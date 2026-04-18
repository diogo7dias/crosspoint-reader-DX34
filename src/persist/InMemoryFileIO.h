#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include "IFileIO.h"

namespace crosspoint {
namespace persist {

// Host-side IFileIO backed by std::unordered_map. Mirrors the tmp/bak/real
// atomic-write state machine so tests can simulate crash-at-step-N by
// injecting a failure via setFailNextWriteAt.
class InMemoryFileIO : public IFileIO {
 public:
  bool safeWrite(const std::string& path, const std::string& content) override;
  bool safeWriteStreamed(const std::string& path, const StreamProducer& produce) override;
  std::string safeRead(const std::string& path) override;
  bool exists(const std::string& path) override;
  bool mkdir(const std::string& path) override;
  bool copy(const std::string& from, const std::string& to) override;

  // --- Test helpers (not in IFileIO) ---
  // Directly seed a file (e.g. to simulate stale .tmp, or a .bak-only state).
  void put(const std::string& path, const std::string& content);
  void erase(const std::string& path);
  bool has(const std::string& path) const;
  size_t fileCount() const { return files_.size(); }
  size_t writeCount() const { return writeCount_; }
  void resetWriteCount() { writeCount_ = 0; }

  // Inject failure at step N of safeWrite:
  //   1 = writing .tmp
  //   2 = removing stale .bak
  //   3 = rotating real → .bak
  //   4 = promoting .tmp → real
  // Applies once, then self-clears.
  void failNextWriteAtStep(int step) { failStep_ = step; }

 private:
  std::unordered_map<std::string, std::string> files_;
  size_t writeCount_ = 0;
  int failStep_ = 0;
};

}  // namespace persist
}  // namespace crosspoint
