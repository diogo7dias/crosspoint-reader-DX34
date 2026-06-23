#include "FontFamilyApply.h"

#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "fonts/CustomBinFontManager.h"

namespace crosspoint::settings {

FontApplyResult applyFontFamilyByDisplayIndex(std::size_t displayIndex, std::size_t builtinCount) {
  auto& mgr = crosspoint::fonts::CustomBinFontManager::instance();

  // Snapshot every field we might touch so a failed activation reverts atomically
  // — otherwise SETTINGS would point at a font the renderer never accepted and
  // ensureSectionLoaded would re-try the failing activate on every chapter.
  const auto prevFamily = SETTINGS.fontFamily;
  const auto prevName = SETTINGS.customFontName;
  const auto prevSize = SETTINGS.customFontSizePt;
  const auto prevFontSize = SETTINGS.fontSize;

  if (displayIndex < builtinCount) {
    SETTINGS.fontFamily = CrossPointSettings::displayIndexToFontFamily(static_cast<uint8_t>(displayIndex));
    SETTINGS.customFontName.clear();
    SETTINGS.customFontSizePt = 0;
  } else {
    const std::vector<std::string> names = mgr.familyNames();
    const std::size_t slot = displayIndex - builtinCount;
    if (slot >= names.size()) {
      return {false, false};  // stale index (font uninstalled mid-pick) — no-op
    }
    SETTINGS.fontFamily = CrossPointSettings::CUSTOM_FAMILY;
    SETTINGS.customFontName = names[slot];
    const std::vector<uint8_t> sizes = mgr.installedSizesFor(SETTINGS.customFontName);
    // Keep the previously-selected size if it still exists for the new family;
    // otherwise fall back to the smallest installed size.
    bool keep = false;
    for (auto s : sizes) {
      if (s == SETTINGS.customFontSizePt) {
        keep = true;
        break;
      }
    }
    if (!keep) {
      SETTINGS.customFontSizePt = sizes.empty() ? 0 : sizes.front();
    }
  }

  SETTINGS.fontFamily = CrossPointSettings::normalizeFontFamily(SETTINGS.fontFamily);
  SETTINGS.fontSize = CrossPointSettings::normalizeFontSizeForFamily(SETTINGS.fontFamily, SETTINGS.fontSize);
  // lineSpacingPercent deliberately left as-is (see header).

  bool ok = true;
  if (SETTINGS.fontFamily == CrossPointSettings::CUSTOM_FAMILY && !SETTINGS.customFontName.empty()) {
    // activate() is atomic: on failure the previously active font stays registered.
    ok = mgr.activate(SETTINGS.customFontName, SETTINGS.customFontSizePt);
  } else {
    mgr.deactivate();
  }

  if (!ok) {
    SETTINGS.fontFamily = prevFamily;
    SETTINGS.customFontName = prevName;
    SETTINGS.customFontSizePt = prevSize;
    SETTINGS.fontSize = prevFontSize;
    return {false, false};
  }

  const bool changed =
      prevFamily != SETTINGS.fontFamily || prevName != SETTINGS.customFontName || prevSize != SETTINGS.customFontSizePt;
  return {true, changed};
}

}  // namespace crosspoint::settings
