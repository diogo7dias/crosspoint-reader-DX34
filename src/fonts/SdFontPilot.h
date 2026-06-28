#pragma once

#include <EpdFont.h>
#include <EpdFontData.h>

#include <cstdint>

namespace crosspoint {
namespace fonts {

// Phase-1 pilot for SD-backed reader fonts. For the given font:
//   1) if `path` is missing on SD, export it from the in-flash bitmap blob;
//   2) open + validate the pack against this firmware's tables;
//   3) on success, repoint `font.data` at an SD-backed copy whose bitmap streams
//      from SD (the glyph/kerning tables stay in flash).
//
// On ANY failure (no SD, write/read error, validation mismatch) the font keeps
// its flash-resident bitmap, so reading never breaks — the zero-risk Phase-1
// step. In a Phase-2 slim build `flashData` has had its bitmap dropped from
// flash (nullptr); there a failure would otherwise leave the font rendering from
// a null pointer, so `slimFallback` (a guaranteed flash font, e.g. ChareInk) is
// substituted instead. Compiles to a no-op call site unless built with
// -DCROSSPOINT_SD_FONTS.
void bootstrapSdFontPilot(EpdFont& font, const EpdFontData& flashData, const char* path, uint16_t sizePt,
                          const EpdFontData* slimFallback = nullptr);

}  // namespace fonts
}  // namespace crosspoint
