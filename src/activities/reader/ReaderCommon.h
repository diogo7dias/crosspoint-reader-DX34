#pragma once

#include <cstdint>

class GfxRenderer;

namespace ReaderCommon {

// Apply the logical reader orientation to the renderer.
// Shared across reader activities (Epub, Txt) so orientation mapping lives in
// one place.
void applyReaderOrientation(GfxRenderer& renderer, uint8_t orientation);

}  // namespace ReaderCommon
