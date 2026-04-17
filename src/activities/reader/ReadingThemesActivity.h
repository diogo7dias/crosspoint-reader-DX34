#pragma once

#include <functional>
#include <string>

#include "activities/ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

class ReadingThemesActivity final : public ActivityWithSubactivity {
 public:
  explicit ReadingThemesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const std::string& bookCachePath, const std::function<void(bool)>& onClose)
      : ActivityWithSubactivity("ReadingThemes", renderer, mappedInput),
        bookCachePath(bookCachePath),
        onClose(onClose) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int selectedRowIndex = 0;
  bool actionPopupOpen = false;
  int actionPopupThemeIndex = -1;
  int actionPopupSelectedIndex = 0;
  bool messagePopupOpen = false;
  std::string messagePopupText;
  bool settingsDirty = false;
  bool pendingSubactivityExit = false;  // Defer subactivity exit to avoid use-after-free
  bool pendingSettingsChanged = false;
  std::function<void()> pendingPostExitAction;

  std::string bookCachePath;
  const std::function<void(bool)> onClose;

  int rowCount() const;
  bool isThemeRow(int rowIndex) const;
  int themeIndexForRow(int rowIndex) const;
  int clampSelectedRow(int rowIndex) const;
  void showMessage(const std::string& message);
  void openKeyboardForNewTheme();
  void openKeyboardForRename(int themeIndex);
  void executeThemeAction();
};
