#pragma once

#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "activities/settings/EnumOptionPicker.h"
#include "activities/settings/FontFamilyPicker.h"
#include "activities/settings/SettingsActivity.h"
#include "util/ButtonNavigator.h"

// Slim version of SettingInfo for the in-reader settings screen.
// Drops unused fields (key, category, stringPtr, std::function accessors)
// to reduce per-entry heap cost from ~160 bytes to ~40 bytes.
struct ReaderSettingInfo {
  StrId nameId;
  SettingType type;
  uint8_t CrossPointSettings::* valuePtr = nullptr;
  std::vector<StrId> enumValues;
  // Optional dynamic tail appended after enumValues. When non-empty, the
  // indices [enumValues.size() .. enumValues.size() + dynamicLabels.size())
  // render with the raw std::string label rather than an I18N lookup. Used
  // for values that come from user data (e.g. installed custom font names
  // — "Custom: unifont", "Custom: iosevka") which cannot be precompiled
  // into StrId constants.
  std::vector<std::string> dynamicLabels;

  struct ValueRange {
    uint8_t min;
    uint8_t max;
    uint8_t step;
  };
  ValueRange valueRange = {};

  size_t totalOptionCount() const { return enumValues.size() + dynamicLabels.size(); }

  static ReaderSettingInfo Toggle(StrId nameId, uint8_t CrossPointSettings::* ptr) {
    ReaderSettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::TOGGLE;
    s.valuePtr = ptr;
    return s;
  }

  static ReaderSettingInfo Enum(StrId nameId, uint8_t CrossPointSettings::* ptr, std::vector<StrId> values) {
    ReaderSettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.valuePtr = ptr;
    s.enumValues = std::move(values);
    return s;
  }

  static ReaderSettingInfo Value(StrId nameId, uint8_t CrossPointSettings::* ptr, const ValueRange valueRange) {
    ReaderSettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::VALUE;
    s.valuePtr = ptr;
    s.valueRange = valueRange;
    return s;
  }
};

class ReaderSettingsActivity final : public Activity {
 public:
  explicit ReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                  const std::string& bookCachePath, const std::function<void(bool)>& onClose)
      : Activity("ReaderSettings", renderer, mappedInput), bookCachePath(bookCachePath), onClose(onClose) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  struct FlatSettingRow {
    bool isHeader = false;
    int categoryIndex = 0;
    int settingIndex = -1;
  };

  ButtonNavigator buttonNavigator;
  std::vector<ReaderSettingInfo> readerSettings;
  std::vector<ReaderSettingInfo> statusBarSettings;
  std::vector<FlatSettingRow> flatRows;
  int selectedRowIndex = 0;
  bool dirty = false;
  bool fontSizeEditMode = false;
  uint8_t fontSizeEditDraftIndex = 0;
  bool valueEditMode = false;
  int valueEditCategoryIndex = -1;
  int valueEditSettingIndex = -1;
  uint8_t valueEditDraft = 0;
  uint8_t valueEditMin = 0;
  uint8_t valueEditMax = 0;
  // Per-direction timestamp of the last value-edit tap, for quick-double-tap
  // detection (a tap within kValueEditDoubleTapMs jumps by 10 instead of 1).
  // 0 = no prior tap this edit session.
  unsigned long lastValueUpTapMs = 0;
  unsigned long lastValueDownTapMs = 0;

  // Modal font-family picker overlay (opened from the Font Family row).
  crosspoint::settings::FontFamilyPicker fontPicker;

  // Generic modal picker for any other ENUM setting with > 2 options (e.g.
  // status-item positions). Opened from toggleCurrentSetting, applied in loop().
  crosspoint::settings::EnumOptionPicker enumPicker;
  int enumPickerCategoryIndex = -1;
  int enumPickerSettingIndex = -1;

  std::string bookCachePath;
  const std::function<void(bool)> onClose;

  void buildSettingsList();
  bool isTxtContext() const;
  const std::vector<ReaderSettingInfo>* settingsForCategory(int categoryIndex) const;
  int findNextEditableRow(int startIndex, int direction) const;
  bool isPopupValueSetting(const ReaderSettingInfo& setting) const;
  void startFontSizeEdit();
  void adjustFontSizeEdit(int delta);
  void applyFontSizeEdit();
  void startValueEdit(const ReaderSettingInfo& setting, int categoryIndex, int settingIndex);
  void adjustValueEdit(int delta);
  void applyValueEdit();
  void toggleCurrentSetting();
  void openEnumPicker(const ReaderSettingInfo& setting, int categoryIndex, int settingIndex);
  void applyEnumPickerSelection();
  std::string currentValueEditText() const;
  void persistSettings(const char* context);
};
