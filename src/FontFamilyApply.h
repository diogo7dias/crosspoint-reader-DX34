#pragma once

#include <cstddef>

// Shared font-family application used by both the global SettingsActivity and
// the in-reader ReaderSettingsActivity (and the font-picker popup). Centralises
// the display-index -> built-in family resolution and valid-size snapping so the
// two menus cannot drift.
namespace crosspoint::settings {

struct FontApplyResult {
  bool ok;       // false => stale/out-of-range index; SETTINGS untouched
  bool changed;  // true => the resolved family differs from before
};

// Apply the font-family selection identified by a picker display index:
//   [0, builtinCount) -> built-in family via displayIndexToFontFamily()
// All reader fonts are built-in; an index at/after builtinCount is a stale pick
// and is a no-op.
//
// Snaps fontSize to a size valid for the resolved family.
//
// Line spacing (lineSpacingPercent) is intentionally NOT modified: switching
// font family preserves the reader's chosen leading.
FontApplyResult applyFontFamilyByDisplayIndex(std::size_t displayIndex, std::size_t builtinCount);

}  // namespace crosspoint::settings
