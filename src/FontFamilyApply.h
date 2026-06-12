#pragma once

#include <cstddef>

// Shared font-family application used by both the global SettingsActivity and
// the in-reader ReaderSettingsActivity (and the font-picker popup). Centralises
// the display-index -> family resolution, valid-size snapping, custom-font
// activation and atomic revert-on-failure so the two menus cannot drift.
namespace crosspoint::settings {

struct FontApplyResult {
  bool ok;       // false => activation failed and SETTINGS was reverted untouched
  bool changed;  // true => the resolved family/name/size differs from before
};

// Apply the font-family selection identified by a picker display index:
//   [0, builtinCount)            -> built-in family via displayIndexToFontFamily()
//   [builtinCount, builtinCount+N) -> the N installed custom families, in
//                                     CustomBinFontManager::familyNames() order.
//
// Snaps customFontSizePt / fontSize to a size valid for the resolved family,
// activates the custom .bin font (or deactivates for a built-in) and, if
// activation fails, atomically reverts every field it touched.
//
// Line spacing (lineSpacingPercent) is intentionally NOT modified: switching
// font family preserves the reader's chosen leading.
FontApplyResult applyFontFamilyByDisplayIndex(std::size_t displayIndex, std::size_t builtinCount);

}  // namespace crosspoint::settings
