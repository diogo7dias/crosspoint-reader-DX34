#pragma once
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "activities/ActivityWithSubactivity.h"
#include "activities/settings/EnumOptionPicker.h"
#include "activities/settings/FontFamilyPicker.h"
#include "util/ButtonNavigator.h"

class CrossPointSettings;

enum class SettingType { TOGGLE, ENUM, ACTION, VALUE, STRING };

enum class SettingAction {
  None,
  RemapFrontButtons,
  KOReaderSync,
  OPDSBrowser,
  Network,
  ClearCache,
  CheckForUpdates,
  RandomizeSleepImages,
  ViewSleepWallpapers,
  RefreshHomeStats,
  ManageCustomFonts,
  CleanupStorage,
};

struct SettingInfo {
  StrId nameId;
  SettingType type;
  uint8_t CrossPointSettings::* valuePtr = nullptr;
  std::vector<StrId> enumValues;
  // Free-string labels appended after enumValues for ENUM types whose
  // option list grows at runtime — e.g. installed custom font names
  // discovered from /custom-font/. Display indices [0, enumValues.size())
  // resolve via I18N; [enumValues.size(), enumValues.size()+dynamicLabels.size())
  // resolve directly from this vector.
  std::vector<std::string> dynamicLabels;
  SettingAction action = SettingAction::None;

  struct ValueRange {
    uint8_t min;
    uint8_t max;
    uint8_t step;
  };
  ValueRange valueRange = {};

  const char* key = nullptr;             // JSON API key (nullptr for ACTION types)
  StrId category = StrId::STR_NONE_OPT;  // Category for web UI grouping

  // Direct char[] string fields (for settings stored in CrossPointSettings)
  char* stringPtr = nullptr;
  size_t stringMaxLen = 0;

  // Dynamic accessors (for settings stored outside CrossPointSettings, e.g. KOReaderCredentialStore)
  std::function<uint8_t()> valueGetter;
  std::function<void(uint8_t)> valueSetter;
  std::function<std::string()> stringGetter;
  std::function<void(const std::string&)> stringSetter;

  static SettingInfo Toggle(StrId nameId, uint8_t CrossPointSettings::* ptr, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::TOGGLE;
    s.valuePtr = ptr;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Enum(StrId nameId, uint8_t CrossPointSettings::* ptr, std::vector<StrId> values,
                          const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.valuePtr = ptr;
    s.enumValues = std::move(values);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Action(StrId nameId, SettingAction action) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ACTION;
    s.action = action;
    return s;
  }

  static SettingInfo Value(StrId nameId, uint8_t CrossPointSettings::* ptr, const ValueRange valueRange,
                           const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::VALUE;
    s.valuePtr = ptr;
    s.valueRange = valueRange;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo String(StrId nameId, char* ptr, size_t maxLen, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringPtr = ptr;
    s.stringMaxLen = maxLen;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicEnum(StrId nameId, std::vector<StrId> values, std::function<uint8_t()> getter,
                                 std::function<void(uint8_t)> setter, const char* key = nullptr,
                                 StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.enumValues = std::move(values);
    s.valueGetter = std::move(getter);
    s.valueSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicString(StrId nameId, std::function<std::string()> getter,
                                   std::function<void(const std::string&)> setter, const char* key = nullptr,
                                   StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringGetter = std::move(getter);
    s.stringSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }
};

class SettingsActivity final : public ActivityWithSubactivity {
  ButtonNavigator buttonNavigator;

  struct FlatSettingRow {
    bool isHeader = false;
    int categoryIndex = 0;
    int settingIndex = -1;
  };

  int selectedRowIndex = 0;
  unsigned long lastNextTapMs = 0;
  unsigned long lastPreviousTapMs = 0;
  bool homeStatsPopupOpen = false;
  bool homeStatsScanning = false;
  bool randomizePopupOpen = false;
  bool randomizePopupSuccess = false;
  bool messagePopupOpen = false;
  std::string messagePopupText;
  bool fontSizeEditMode = false;
  uint8_t fontSizeEditDraftIndex = 0;
  bool valueEditMode = false;
  int valueEditCategoryIndex = -1;
  int valueEditSettingIndex = -1;
  uint8_t valueEditOriginal = 0;
  uint8_t valueEditDraft = 0;
  uint8_t valueEditMin = 0;
  uint8_t valueEditMax = 0;
  // Per-direction timestamp of the last value-edit tap, for quick-double-tap
  // detection (a tap within kValueEditDoubleTapMs jumps by 10 instead of 1).
  // Separate from the lastNext/PreviousTapMs used for menu category jumps.
  unsigned long valueEditLastUpTapMs = 0;
  unsigned long valueEditLastDownTapMs = 0;

  // Modal font-family picker overlay (opened from the Font Family row).
  crosspoint::settings::FontFamilyPicker fontPicker;

  // Generic modal picker for any other ENUM setting with > 2 options (e.g.
  // status-item positions). Opened from toggleCurrentSetting, applied in loop().
  crosspoint::settings::EnumOptionPicker enumPicker;
  int enumPickerCategoryIndex = -1;
  int enumPickerSettingIndex = -1;

  // Per-category settings derived from shared list + device-only actions
  std::vector<SettingInfo> displaySettings;
  std::vector<SettingInfo> readerSettings;
  std::vector<SettingInfo> statusBarSettings;
  std::vector<SettingInfo> controlsSettings;
  std::vector<SettingInfo> systemSettings;
  std::vector<FlatSettingRow> flatRows;
  std::vector<int> categoryHeaderRowIndices;

  const std::function<void()> onGoHome;

  static constexpr int categoryCount = 5;
  static const StrId categoryNames[categoryCount];

  const std::vector<SettingInfo>* settingsForCategory(int categoryIndex) const;
  int findNextEditableRow(int startIndex, int direction) const;
  void jumpCategory(int direction);
  void buildSettingsList();
  bool isPopupValueSetting(const SettingInfo& setting) const;
  void startFontSizeEdit();
  void adjustFontSizeEdit(int delta);
  void applyFontSizeEdit();
  void startValueEdit(const SettingInfo& setting, int categoryIndex, int settingIndex);
  void adjustValueEdit(int delta);
  void applyValueEdit();
  void cancelValueEdit();
  std::string currentValueEditText() const;
  void toggleCurrentSetting();
  // Open the generic option picker for an ENUM setting with > 2 options.
  void openEnumPicker(const SettingInfo& setting, int categoryIndex, int settingIndex);
  // Apply the option picked in enumPicker to the setting it was opened for.
  void applyEnumPickerSelection();

 public:
  explicit SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                            const std::function<void()>& onGoHome)
      : ActivityWithSubactivity("Settings", renderer, mappedInput), onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;
};
