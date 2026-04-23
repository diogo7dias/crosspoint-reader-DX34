#pragma once

#include <cstdint>

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

class BdfIndexBuilder {
 public:
  // Open the BDF at `bdfPath`, walk every glyph, and write a sorted .idx
  // file to `idxPath`. Existing .idx is overwritten atomically (.tmp →
  // rename). The BDF is required to have glyphs in ascending codepoint
  // order — out-of-order entries cause the build to abort with an error.
  static BuildIndexResult buildIndex(const char* bdfPath, const char* idxPath);
};

}  // namespace bdf
}  // namespace crosspoint
