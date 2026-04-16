#pragma once

class GfxRenderer;

// Lightweight visual feedback for activity transitions.
// show() draws a top-aligned label instantly (HALF_REFRESH).
// Multiple concurrent popups stack downward automatically.
// dismiss() does a HALF_REFRESH to cleanly clear any ghosting before
// the next screen is drawn.
namespace TransitionFeedback {

void show(GfxRenderer& renderer, const char* message);
void dismiss(GfxRenderer& renderer);
bool isActive();

// Bottom edge (Y) of the last drawn popup, for stacking other popups below.
int bottomY();

}  // namespace TransitionFeedback
