// Hyphenator stub for the host reader-sim. The slice runs with hyphenation
// disabled, but ParsedText.cpp references these symbols, so provide no-op
// definitions to satisfy the linker (real patterns/dictionaries are not pulled
// into the sim).
#include "hyphenation/Hyphenator.h"  // -I lib/Epub/Epub

const LanguageHyphenator* Hyphenator::cachedHyphenator_ = nullptr;

// Synthetic hyphenation for the sim: insert a break roughly every 4 bytes for
// words longer than 8 bytes. We don't care about linguistic correctness here —
// the point is to drive ParsedText::expandHyphenationBreaks' allocation path
// (one of the v3.0.1 bad_alloc hotfix sites) under SimHeap fragmentation.
std::vector<Hyphenator::BreakInfo> Hyphenator::breakOffsets(const std::string& word, bool) {
  std::vector<BreakInfo> out;
  if (word.size() <= 8) return out;
  for (size_t i = 4; i + 4 < word.size(); i += 4) {
    out.push_back(BreakInfo{i, /*requiresInsertedHyphen=*/true});
  }
  return out;
}

void Hyphenator::setPreferredLanguage(const std::string&) {}
