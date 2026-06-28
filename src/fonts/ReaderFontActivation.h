#pragma once

namespace crosspoint {
namespace fonts {

// Ensures the SD-backed CPBN packs for `fontId`'s weight variants are open before
// the renderer measures or draws any glyph. No-op (and effectively zero cost)
// unless the firmware was built with -DCROSSPOINT_SD_FONTS. Safe to call on every
// page render: the manager only touches the SD card when the active reader font
// actually changes, so a page-turn within the same font does no I/O.
//
// This thin declaration is all the reader activities depend on, keeping the
// SD-font headers out of their translation units in the default build.
void activateReaderFont(int fontId);

}  // namespace fonts
}  // namespace crosspoint
