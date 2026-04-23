#pragma once

#include <cstdint>
#include <functional>

namespace crosspoint {
namespace bdf {

struct BuildIndexResult {
  uint32_t glyphsWritten = 0;
  uint32_t glyphsSkipped = 0;
  uint32_t parseTimeMs = 0;
  bool ok = false;
  // Static literal — never owned. nullptr on success.
  const char* error = nullptr;
};

// Optional progress callback. `written` is the count of index entries
// flushed so far; `total` is the CHARS count read from the BDF header
// (upper bound on glyphs yielded; may be slightly higher than the final
// written count because glyphs with invalid ENCODINGs are skipped).
// Fires at most every N glyphs written (builder-tunable below).
using BuildProgressCallback = std::function<void(uint32_t written, uint32_t total)>;

class BdfIndexBuilder {
 public:
  // Open the BDF at `bdfPath`, walk every glyph, and write a sorted .idx
  // file to `idxPath`. Existing .idx is overwritten atomically (.tmp →
  // rename). The BDF is required to have glyphs in ascending codepoint
  // order — out-of-order entries cause the build to abort with an error.
  //
  // Optional `progress` callback fires every `progressEveryN` glyphs so
  // UI code can draw a live bar; default 256 keeps callback overhead
  // negligible (~200 calls for a 57 k-glyph BDF). Passing nullptr or
  // progressEveryN=0 disables progress reporting.
  static BuildIndexResult buildIndex(const char* bdfPath, const char* idxPath,
                                     BuildProgressCallback progress = nullptr, uint32_t progressEveryN = 256);
};

}  // namespace bdf
}  // namespace crosspoint
