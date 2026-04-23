#include "CustomFontInstallProgressActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "components/themes/BaseTheme.h"
#include "fontIds.h"

void CustomFontInstallProgressActivity::onEnter() {
  Activity::onEnter();
  // Previous activity (install prompt) leaves ghost pixels on e-ink fast
  // refresh. A half refresh clears to white before the progress frame draws.
  renderer.requestHalfRefresh();
  requestUpdate();
}

void CustomFontInstallProgressActivity::render(Activity::RenderLock&&) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  const int cy = pageHeight / 2;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);

  // Minimal install screen — big "Installing..." + filename + reboot warning.
  // E-ink fast-refresh ghosts any progress bar into mush, so we skip the bar
  // entirely and trust that the user sees the screen change + "Do not reboot".
  renderer.drawCenteredText(UI_12_FONT_ID, cy - lineH * 2, tr(STR_INSTALLING), true, EpdFontFamily::BOLD);

  const std::string trimmed = renderer.truncatedText(UI_10_FONT_ID, filename.c_str(), pageWidth - 40);
  renderer.drawCenteredText(UI_10_FONT_ID, cy, trimmed.c_str(), true);

  renderer.drawCenteredText(UI_10_FONT_ID, cy + lineH * 2, tr(STR_INSTALL_DO_NOT_REBOOT), true);

  renderer.displayBuffer();
}
