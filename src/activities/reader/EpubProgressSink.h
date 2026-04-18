/**
 * @file EpubProgressSink.h
 * @brief SD-backed IProgressSink producing byte-identical EPUB progress.bin.
 *
 * Wraps the atomic-write pattern previously inlined in
 * EpubReaderActivity::saveProgress (lines 1610-1656 before RFC #21 stage 1):
 *   write → /<cache>/progress_tmp.bin
 *   remove /<cache>/progress.bin
 *   rename tmp → progress.bin
 *
 * Layout (6 bytes LE): spineIndex[u16], page[u16], pageCount[u16]. Clamps
 * spineIndex into [0, spineCount); if clamped to spineCount-1, page → UINT16_MAX
 * (legacy "past end" sentinel). pageCount <= 0 coerced to 1.
 *
 * Device-only — pulls HalStorage. Host tests use a fake sink.
 */
#pragma once

#include <string>

#include "ReaderProgressTracker.h"

namespace crosspoint {
namespace reader {

class EpubProgressSink : public IProgressSink {
 public:
  // `cachePath` is the per-book cache dir (no trailing slash), e.g.
  // /.crosspoint/cache/<fingerprint>. `spineCount` is the total number of
  // spine items — used for bounds clamping matching legacy behaviour.
  EpubProgressSink(std::string cachePath, int spineCount) : cachePath_(std::move(cachePath)), spineCount_(spineCount) {}

  // IProgressSink
  bool write(const ReaderPosition& p) override;

  void setSpineCount(int n) { spineCount_ = n; }
  void setCachePath(std::string path) { cachePath_ = std::move(path); }

 private:
  std::string cachePath_;
  int spineCount_;
};

}  // namespace reader
}  // namespace crosspoint
