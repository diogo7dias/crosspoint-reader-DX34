#include "LanguageRegistry.h"

#include <algorithm>
#include <array>

#include "HyphenationCommon.h"
#include "generated/hyph-en.trie.h"
#include "generated/hyph-pt.trie.h"

namespace {

// English hyphenation patterns (3/3 minimum prefix/suffix length)
LanguageHyphenator englishHyphenator(en_patterns, isLatinLetter, toLowerLatin, 3, 3);
// Portuguese (pt / pt-PT / pt-BR), 2/2 minimum prefix/suffix (Liang default, matches
// upstream #2209 and the pt pattern set's test data). Denser hyphenation than 2/3
// tightens justification on the narrow e-ink column, which matters more here than the
// babel righthyphenmin=3 nicety.
LanguageHyphenator portugueseHyphenator(pt_patterns, isLatinLetter, toLowerLatin);

using EntryArray = std::array<LanguageEntry, 2>;

const EntryArray& entries() {
  static const EntryArray kEntries = {{{"english", "en", &englishHyphenator},
                                       {"portuguese", "pt", &portugueseHyphenator}}};
  return kEntries;
}

}  // namespace

const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag) {
  const auto& allEntries = entries();
  const auto it = std::find_if(allEntries.begin(), allEntries.end(),
                               [&primaryTag](const LanguageEntry& entry) { return primaryTag == entry.primaryTag; });
  return (it != allEntries.end()) ? it->hyphenator : nullptr;
}

LanguageEntryView getLanguageEntries() {
  const auto& allEntries = entries();
  return LanguageEntryView{allEntries.data(), allEntries.size()};
}
