#pragma once

#include <cstdint>
#include <string>

class GfxRenderer;

namespace ReaderCommon {

// Apply the logical reader orientation to the renderer.
// Shared across reader activities (Epub, Txt) so orientation mapping lives in
// one place.
void applyReaderOrientation(GfxRenderer& renderer, uint8_t orientation);

// Format the status-bar page counter text based on the configured mode.
// Shared between Epub and Txt readers. Epub passes chapter page count here;
// Txt passes its total page count.
std::string formatPageCounterText(uint8_t mode, int currentPage, int totalPages);

}  // namespace ReaderCommon
