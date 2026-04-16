#pragma once

#include <WString.h>

#include <string>

class GfxRenderer;

namespace StatusPopup {

// Show a popup that stays on screen (e.g. "Moving file").
void showBlocking(GfxRenderer& renderer, const std::string& message);
void showBlocking(GfxRenderer& renderer, const char* message);
void showBlocking(GfxRenderer& renderer, const String& message);

// Show a brief confirmation popup (e.g. "Moved"), hold for ~1 s,
// then clear the popup stack so the next redraw starts clean.
void showConfirmation(GfxRenderer& renderer, const char* message);

}  // namespace StatusPopup
