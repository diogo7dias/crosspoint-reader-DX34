#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace crosspoint {
namespace bdf {

// Variant tags parsed from the tail of the family name portion of the
// filename (before the `_<size>` suffix). Matches EpdFontFamily::Style bit
// convention so a Variant value cast to uint8_t is directly comparable.
enum class BdfVariant : uint8_t {
  Regular = 0,
  Bold = 1,
  Italic = 2,
  BoldItalic = 3,
};

struct BdfFilenameInfo {
  std::string name;            // family name WITHOUT the -bold / -italic tag
  uint16_t sizePt;
  BdfVariant variant = BdfVariant::Regular;
};

// Parses "<name>[-<variant>]_<size>.bdf" (extension case-insensitive,
// variant tag case-insensitive).
//   name chars:     [A-Za-z0-9-]+    (at least one non-variant char)
//   variant tokens: "bold" | "italic" | "bolditalic"   (optional)
//   size:           4..255 inclusive (decimal digits only)
// Returns nullopt if the basename does not match.
//
// Examples:
//   "unifont_16.bdf"               -> {"unifont", 16, Regular}
//   "unifont-bold_16.bdf"          -> {"unifont", 16, Bold}
//   "unifont-italic_16.bdf"        -> {"unifont", 16, Italic}
//   "unifont-bolditalic_16.bdf"    -> {"unifont", 16, BoldItalic}
//   "my-font-bold_24.bdf"          -> {"my-font", 24, Bold}
//   "Unifont-BOLD_16.BDF"          -> {"Unifont", 16, Bold}
//   "-bold_16.bdf"                 -> nullopt  (empty family name)
//   "foo_3.bdf"                    -> nullopt  (size < 4)
std::optional<BdfFilenameInfo> parseBdfFilename(const char* basename);

}  // namespace bdf
}  // namespace crosspoint
