#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace crosspoint {
namespace bdf {

struct BdfFilenameInfo {
  std::string name;
  uint16_t sizePt;
};

// Parses "<name>_<size>.bdf" (extension case-insensitive).
//   name chars: [A-Za-z0-9-]+
//   size:       4..255 inclusive (decimal digits only)
// Returns nullopt if the basename does not match.
//
// Examples:
//   "unifont_16.bdf"   -> {"unifont", 16}
//   "Unifont_16.BDF"   -> {"Unifont", 16}
//   "my-font_24.bdf"   -> {"my-font", 24}
//   "unifont.bdf"      -> nullopt  (no _<digits>)
//   "_16.bdf"          -> nullopt  (empty name)
//   "foo_3.bdf"        -> nullopt  (size < 4)
//   "foo_300.bdf"      -> nullopt  (size > 255)
std::optional<BdfFilenameInfo> parseBdfFilename(const char* basename);

}  // namespace bdf
}  // namespace crosspoint
