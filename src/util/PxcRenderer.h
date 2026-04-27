#pragma once

#include <GfxRenderer.h>

#include <string>

namespace PxcRenderer {

// Streams a screen-sized .pxc into the LSB/MSB grayscale planes and pushes
// the composite via the supplied `mode`. Returns false on open / header /
// size-mismatch / row-buffer-alloc failure. The caller picks the mode:
//
//  - FactoryQuality / FactoryFast: pre-flashes white via HALF_REFRESH then
//    drives the panel absolutely with the LUT. Best image quality, ~2.2 s.
//    Suitable when nothing else is on screen (sleep wallpaper without
//    overlays).
//  - Differential: composites on top of whatever the panel already shows.
//    Skips the pre-flash, ~0.5 s faster. Required when a BW base layer
//    (e.g. button hints) was just pushed and must remain visible under
//    the grayscale image.
//
// Does NOT call displayBuffer afterward — the grayscale composite is
// pushed inside renderGrayscale via displayGrayBuffer.
//
// PXC layout: uint16_t width, uint16_t height, then packed 2 bpp payload
// (4 px/byte, MSB first). Pixel convention: 0=Black, 1=DarkGray,
// 2=LightGray, 3=White (matches Bitmap::readNextRow).
bool renderPxc(GfxRenderer& renderer, const std::string& path, GfxRenderer::GrayscaleMode mode);

// Streams the PXC and writes a 1-bit BW representation into the renderer's
// current frameBuffer. Pixels with pv < 3 (Black/DarkGray/LightGray) are
// drawn black; pv == 3 (White) is left as background. Caller is responsible
// for clearScreen() before and displayBuffer() after — this just populates
// the frameBuffer. Used to prime the panel before a Differential grayscale
// overlay so stark Black pixels transition cleanly (Differential mode
// encodes Black and White identically as LSB=0,MSB=0, so it relies on the
// pre-existing panel state to disambiguate them).
bool streamPxcAsBw(GfxRenderer& renderer, const std::string& path);

}  // namespace PxcRenderer
