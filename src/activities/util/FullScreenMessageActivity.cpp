#include "FullScreenMessageActivity.h"

#include <GfxRenderer.h>

#include <vector>

#include "MappedInputManager.h"
#include "fontIds.h"

void FullScreenMessageActivity::onEnter() {
  Activity::onEnter();

  // Split message on '\n' so multi-line strings render as separate, centered
  // lines instead of drawing literal LF as a missing-glyph box.
  std::vector<std::string> lines;
  size_t start = 0;
  while (start <= text.size()) {
    auto nl = text.find('\n', start);
    if (nl == std::string::npos) nl = text.size();
    lines.emplace_back(text.substr(start, nl - start));
    start = nl + 1;
  }
  if (lines.empty()) lines.emplace_back("");

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int lineGap = 2;
  const int blockH = static_cast<int>(lines.size()) * lineH + (static_cast<int>(lines.size()) - 1) * lineGap;
  const int top = (renderer.getScreenHeight() - blockH) / 2;

  renderer.clearScreen();
  int y = top;
  const int maxLineW = renderer.getScreenWidth() - 16;  // small horizontal margin
  for (const auto& raw : lines) {
    const std::string truncated = renderer.truncatedText(UI_10_FONT_ID, raw.c_str(), maxLineW);
    renderer.drawCenteredText(UI_10_FONT_ID, y, truncated.c_str(), true, style);
    y += lineH + lineGap;
  }
  renderer.displayBuffer(refreshMode);
}

void FullScreenMessageActivity::loop() {
  if (!onDismiss) return;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    auto cb = onDismiss;
    onDismiss = nullptr;  // guard against re-entry from the parent's exit path
    cb();
  }
}
