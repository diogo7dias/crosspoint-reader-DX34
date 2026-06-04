/**
 * @file PagedProgressSink.h
 * @brief SD-backed IProgressSink for single-document readers (TXT/XTC).
 *
 * Wraps the atomic-write pattern previously inlined in
 * TxtReaderActivity::saveProgress / XtcReaderActivity::saveProgress:
 *   write → /<cache>/progress_tmp.bin
 *   rotate /<cache>/progress.bin → progress.bin.bak
 *   rename tmp → progress.bin
 *
 * Layout (4 bytes LE): page[u32]. Single-document formats leave
 * ReaderPosition.spineIndex = 0, pageCount = 1; only `page` is persisted, so
 * the on-disk bytes are identical to the legacy TXT/XTC progress.bin.
 *
 * Sibling to EpubProgressSink (6-byte). Device-only — pulls HalStorage; the
 * write is synchronous (matching the legacy TXT/XTC path, which never used the
 * AsyncWriter the EPUB sink does). Host tests of the debounce/dirty logic use
 * the ReaderProgressTracker fake sink, not this class (RFC #171).
 */
#pragma once

#include <string>

#include "ReaderProgressTracker.h"

namespace crosspoint {
namespace reader {

class PagedProgressSink : public IProgressSink {
 public:
  // `cachePath` is the per-book cache dir (no trailing slash). `logTag` is the
  // 3-letter subsystem tag for log lines ("TRS" / "XTR"), preserving the
  // legacy per-reader logging.
  PagedProgressSink(std::string cachePath, const char* logTag)
      : cachePath_(std::move(cachePath)), logTag_(logTag) {}

  // IProgressSink — writes ReaderPosition.page as 4-byte LE, synchronously.
  bool write(const ReaderPosition& p) override;

  void setCachePath(std::string path) { cachePath_ = std::move(path); }

  // Load the saved page with progress.bin → .bak → mirror recovery, accepting
  // the legacy 2-byte format too. Returns the raw page (>= 0), or -1 when no
  // progress file is recoverable. The caller clamps to its own page count
  // (TXT uses totalPages, XTC uses getPageCount()).
  static int load(const std::string& cachePath, const char* logTag);

 private:
  std::string cachePath_;
  const char* logTag_;
};

}  // namespace reader
}  // namespace crosspoint
