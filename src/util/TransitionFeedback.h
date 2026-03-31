#pragma once

class GfxRenderer;

// Lightweight visual feedback for activity transitions.
// show() draws a centered label with HALF_REFRESH, then requests a
// HALF_REFRESH for the next screen to clear residual ghosting.
// dismiss() does a HALF_REFRESH to cleanly clear any ghosting before
// the next screen is drawn.
namespace TransitionFeedback {

void show(GfxRenderer& renderer, const char* message);
void dismiss(GfxRenderer& renderer);
bool isActive();

}  // namespace TransitionFeedback
