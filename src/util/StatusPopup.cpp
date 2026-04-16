#include "StatusPopup.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>

#include "StringUtils.h"
#include "TransitionFeedback.h"
#include "components/UITheme.h"

namespace StatusPopup {
namespace {

void showBlockingImpl(GfxRenderer& renderer, const std::string& message) {
  if (message.empty()) {
    return;
  }
  const std::string uppercaseMessage = StringUtils::toUpperAscii(message);
  GUI.drawPopup(renderer, uppercaseMessage.c_str());
}

}  // namespace

void showBlocking(GfxRenderer& renderer, const std::string& message) {
  showBlockingImpl(renderer, message);
}

void showBlocking(GfxRenderer& renderer, const char* message) {
  showBlockingImpl(renderer, message ? std::string(message) : std::string());
}

void showBlocking(GfxRenderer& renderer, const String& message) {
  showBlockingImpl(renderer, std::string(message.c_str()));
}

}  // namespace StatusPopup
