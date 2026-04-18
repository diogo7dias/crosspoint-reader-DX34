#pragma once

#include "IFileIO.h"

namespace crosspoint {
namespace persist {

// Production IFileIO backed by HalStorage. Atomic write sequence
// (tmp → bak → real) matches JsonSettingsIO::safeWriteFile byte-for-byte,
// including the stuck-.tmp fallback to .tmp2 (MEMORY.md bug fix).
class SdFatFileIO : public IFileIO {
 public:
  bool safeWrite(const std::string& path, const std::string& content) override;
  bool safeWriteStreamed(const std::string& path, const StreamProducer& produce) override;
  std::string safeRead(const std::string& path) override;
  bool exists(const std::string& path) override;
  bool mkdir(const std::string& path) override;
  bool copy(const std::string& from, const std::string& to) override;
};

}  // namespace persist
}  // namespace crosspoint
