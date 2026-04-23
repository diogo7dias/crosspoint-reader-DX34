#pragma once

#include <functional>
#include <string>
#include <vector>

#include "activities/ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

// Settings sub-screen that lists every custom font installed under
// /custom-font/ and offers a Delete action on the selected family. The
// list is rebuilt every time onEnter fires so Slice-2 per-variant
// additions surface without reboot. Reinstall is not surfaced here — it
// happens automatically when the user re-drops the .bdf on SD and
// reboots, via the existing CustomFontPromptActivity flow.
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

  void rebuildRows();
  void handleSelection();
};
