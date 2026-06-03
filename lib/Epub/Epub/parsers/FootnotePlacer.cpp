#include "FootnotePlacer.h"

void FootnotePlacer::registerFootnote(int wordIndex, const FootnoteEntry& entry) {
  pending_.push_back({wordIndex, entry});
}

void FootnotePlacer::placeForLine(int lineWordCount, const EmitFn& emit) {
  cumulativeWords_ += lineWordCount;
  auto it = pending_.begin();
  while (it != pending_.end() && it->first <= cumulativeWords_) {
    emit(it->second.number, it->second.href);
    ++it;
  }
  pending_.erase(pending_.begin(), it);
}

void FootnotePlacer::drainRemaining(const EmitFn& emit) {
  for (const auto& [idx, fn] : pending_) {
    (void)idx;
    emit(fn.number, fn.href);
  }
  pending_.clear();
}

void FootnotePlacer::onNewBlock() { cumulativeWords_ = 0; }

void FootnotePlacer::reset() {
  pending_.clear();
  cumulativeWords_ = 0;
}
