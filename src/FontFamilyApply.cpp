#include "FontFamilyApply.h"

#include "CrossPointSettings.h"

namespace crosspoint::settings {

FontApplyResult applyFontFamilyByDisplayIndex(std::size_t displayIndex, std::size_t builtinCount) {
  // All reader fonts are now built-in (flash-resident); there is no custom-font
  // region beyond the built-in labels. Any displayIndex at/after builtinCount is
  // a stale pick and is a no-op.
  if (displayIndex >= builtinCount) {
    return {false, false};
  }

  const auto prevFamily = SETTINGS.fontFamily;

  SETTINGS.fontFamily = CrossPointSettings::normalizeFontFamily(
      CrossPointSettings::displayIndexToFontFamily(static_cast<uint8_t>(displayIndex)));
  SETTINGS.fontSize = CrossPointSettings::normalizeFontSizeForFamily(SETTINGS.fontFamily, SETTINGS.fontSize);
  // lineSpacingPercent deliberately left as-is (see header).

  return {true, prevFamily != SETTINGS.fontFamily};
}

}  // namespace crosspoint::settings
