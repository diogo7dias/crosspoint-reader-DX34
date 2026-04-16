#include "StatusPopup.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>

#include "TransitionFeedback.h"

namespace StatusPopup {
namespace {

void showBlockingImpl(GfxRenderer& renderer, const char* message) {
  if (!message || message[0] == '\0') {
    return;
  }
  // Delegates to TransitionFeedback so sequential blocking popups
  // stack correctly instead of overlapping.
  TransitionFeedback::show(renderer, message);
}

}  // namespace

void showBlocking(GfxRenderer& renderer, const std::string& message) {
  showBlockingImpl(renderer, message.c_str());
}

void showBlocking(GfxRenderer& renderer, const char* message) {
  showBlockingImpl(renderer, message);
}

void showBlocking(GfxRenderer& renderer, const String& message) {
  showBlockingImpl(renderer, message.c_str());
}

void showConfirmation(GfxRenderer& renderer, const char* message) {
  // Stack below the progress popup, hold 1s, then clear for next redraw.
  showBlockingImpl(renderer, message);
  delay(1000);
  TransitionFeedback::resetStacking();
}

}  // namespace StatusPopup
