#pragma once

#include <functional>
#include <string>
#include <vector>

#include "activities/ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

// Settings sub-screen that lists every .bin custom font installed under
// /custom-font/<family>/ and offers Delete Size / Delete Family actions
// on the selected row. The list is rebuilt every time onEnter fires so
// newly-uploaded fonts from the web UI surface without reboot.
// Installation is entirely web-server driven (POST /api/fonts/upload);
// there is no device-side install flow.
class CustomFontsSettingsActivity final : public ActivityWithSubactivity {
 public:
  explicit CustomFontsSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::function<void()>& onBack)
      : ActivityWithSubactivity("CustomFontsSettings", renderer, mappedInput), onBack(onBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  struct Row {
    std::string fontName;
    uint16_t sizePt;
    bool hasRegular;
    bool hasBold;
    bool hasItalic;
    bool hasBoldItalic;
  };

  ButtonNavigator buttonNavigator;
  std::vector<Row> rows;
  int selectedIndex = 0;
  const std::function<void()> onBack;

  // When true, the screen shows a 3-option action menu (Delete Size /
  // Delete Family / Cancel) layered over the list selection. Kept in-
  // activity rather than pushed as a separate ConfirmDialog so the list
  // row stays visible underneath via the title, giving the user a clear
  // "what am I about to destroy?" anchor. Back cancels.
  bool inActionMenu = false;
  int actionIndex = 0;

  void rebuildRows();
  void openActionMenu();
  void closeActionMenu();
  void runDeleteSize();
  void runDeleteFamily();
};
