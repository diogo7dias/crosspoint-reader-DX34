/**
 * @file ReaderProgressTracker.h
 * @brief Debounce + dirty-tracking for reader progress persistence.
 *
 * Design (RFC #21 Stage 1 — EPUB reader decomposition, code-only):
 *   - Owns the 6 lastObserved/lastSaved fields + progressDirty + debounce clock
 *     that were scattered across EpubReaderActivity (9 call sites, 7-line dance
 *     at each site with easy-to-miss bookkeeping).
 *   - Format-neutral: Position carries {spineIndex, page, pageCount}; single-page
 *     formats (TXT/XTC) leave spineIndex=0, pageCount=1. Byte layout of
 *     progress.bin lives in the Sink, not here.
 *   - Host-testable: IProgressSink is a pure virtual (prod writes SD via HalStorage,
 *     tests use an in-memory vector). No SdFat, no Arduino, no millis().
 *   - Caller passes nowMs every call. Tracker does not read the clock itself.
 *
 * Intentionally NOT wired into EpubReaderActivity in this stage. Stage 2 (next
 * PR) integrates behind READER_V2 gate with V1 parallel; stage 3 migrates
 * TxtReaderActivity + XtcReaderActivity which duplicate the same pattern.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace crosspoint {
namespace reader {

struct ReaderPosition {
  int32_t spineIndex = -1;
  int32_t page = -1;
  int32_t pageCount = -1;

  bool operator==(const ReaderPosition& o) const {
    return spineIndex == o.spineIndex && page == o.page && pageCount == o.pageCount;
  }
  bool operator!=(const ReaderPosition& o) const { return !(*this == o); }
};

// Sink = "where do the bytes go". Production impl (EpubProgressSink) writes
// the 6-byte progress.bin via HalStorage; host tests use a fake that records
// into std::vector<ReaderPosition>.
struct IProgressSink {
  virtual ~IProgressSink() = default;
  // Write the position. Returns true on success. Called at most once per
  // tracker flush; sink is responsible for atomic rename / crash safety.
  virtual bool write(const ReaderPosition& p) = 0;
};

class ReaderProgressTracker {
 public:
  static constexpr uint32_t kDefaultDebounceMs = 800;

  explicit ReaderProgressTracker(IProgressSink& sink, uint32_t debounceMs = kDefaultDebounceMs)
      : sink_(sink), debounceMs_(debounceMs) {}

  // Seed after boot-time load. Clears dirty; both observed and saved snap to
  // `loaded`. No write performed.
  void seed(const ReaderPosition& loaded);

  // Report what the reader just rendered. If it differs from lastSaved, the
  // tracker becomes dirty and the debounce timer (anchored at nowMs) starts.
  // Idempotent when `rendered` matches lastObserved.
  void observe(const ReaderPosition& rendered, uint32_t nowMs);

  // Flush to the sink if dirty AND (force OR debounce elapsed). Returns true
  // iff a write was performed AND succeeded. On success, lastSaved = last
  // observed and dirty is cleared.
  bool flush(uint32_t nowMs, bool force);

  // Bridge for out-of-band resets (e.g. KOReaderSync pulling a new position
  // from the server): flushes any pending write first (force), then reseeds
  // lastObserved + lastSaved to `p`. After this call, dirty is false.
  void snapshotForReset(const ReaderPosition& p, uint32_t nowMs);

  bool dirty() const { return dirty_; }
  const ReaderPosition& lastObserved() const { return lastObserved_; }
  const ReaderPosition& lastSaved() const { return lastSaved_; }
  uint32_t debounceMs() const { return debounceMs_; }
  size_t flushCount() const { return flushCount_; }

  void setDebounceMs(uint32_t ms) { debounceMs_ = ms; }

 private:
  IProgressSink& sink_;
  uint32_t debounceMs_;
  ReaderPosition lastObserved_{};
  ReaderPosition lastSaved_{};
  bool dirty_ = false;
  uint32_t lastChangeMs_ = 0;
  size_t flushCount_ = 0;
};

}  // namespace reader
}  // namespace crosspoint
