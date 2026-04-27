#pragma once

#include <string>

class GfxRenderer;

namespace PxcRenderer {

// Streams a screen-sized .pxc into the framebuffer using the configured
// grayscale mode (FactoryQuality if SETTINGS.useFactoryLUT, else
// Differential). Returns false if the file cannot be opened, the header
// cannot be read, the declared dimensions diverge from the current screen
// by more than 1 px, or the per-row buffer cannot be allocated.
//
// Does NOT call displayBuffer — caller picks the refresh mode after this
// returns.
//
// PXC layout: uint16_t width, uint16_t height, then packed 2 bpp payload
// (4 px/byte, MSB first). Pixel convention: 0=Black, 1=DarkGray,
// 2=LightGray, 3=White (matches Bitmap::readNextRow).
bool renderPxc(GfxRenderer& renderer, const std::string& path);

}  // namespace PxcRenderer
