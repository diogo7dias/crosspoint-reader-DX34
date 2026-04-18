#include "ReaderProgressTracker.h"

namespace crosspoint {
namespace reader {

void ReaderProgressTracker::seed(const ReaderPosition& loaded) {
  lastObserved_ = loaded;
  lastSaved_ = loaded;
  dirty_ = false;
  lastChangeMs_ = 0;
}

void ReaderProgressTracker::observe(const ReaderPosition& rendered, uint32_t nowMs) {
  if (rendered == lastObserved_) {
    return;
  }
  lastObserved_ = rendered;
  if (rendered != lastSaved_) {
    dirty_ = true;
    lastChangeMs_ = nowMs;
  } else {
    // User navigated back to the last-persisted position before the debounce
    // window fired — nothing left to write.
    dirty_ = false;
  }
}

bool ReaderProgressTracker::flush(uint32_t nowMs, bool force) {
  if (!dirty_) {
    return false;
  }
  if (!force && (nowMs - lastChangeMs_) < debounceMs_) {
    return false;
  }
  if (!sink_.write(lastObserved_)) {
    // Leave dirty set so next flush retries. Don't advance lastSaved.
    return false;
  }
  lastSaved_ = lastObserved_;
  dirty_ = false;
  flushCount_++;
  return true;
}

void ReaderProgressTracker::snapshotForReset(const ReaderPosition& p, uint32_t nowMs) {
  // Preserve any pending write before we clobber observed/saved; otherwise a
  // debounced page-turn could be dropped on KOReader pull.
  flush(nowMs, /*force=*/true);
  lastObserved_ = p;
  lastSaved_ = p;
  dirty_ = false;
  lastChangeMs_ = nowMs;
}

}  // namespace reader
}  // namespace crosspoint
