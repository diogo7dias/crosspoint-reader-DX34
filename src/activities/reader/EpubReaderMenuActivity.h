#pragma once
#include <Epub.h>
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

class EpubReaderMenuActivity final : public ActivityWithSubactivity {
 public:
  // Menu actions available from the reader menu.
  enum class MenuAction {
    NONE,
    SELECT_CHAPTER,
    FOOTNOTES,
    ROTATE_SCREEN,
    THEMES_MENU,
    REVERT_THEME,
    GO_HOME,
    SYNC,
    DELETE_CACHE,
    DELETE_BOOK,
    REMOVE_FROM_RECENT,
    TRIAGE_FAVORITE,
    TRIAGE_PAUSE_ROTATION,
    TRIAGE_MOVE_PAUSE,
    TRIAGE_DELETE,
    TOGGLE_RANDOM_BOOK_ON_BOOT,
  };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const bool hasFootnotes,
                                  const std::function<void(uint8_t)>& onBack,
                                  const std::function<void(MenuAction)>& onAction);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
    const char* literalLabel = nullptr;
    bool isSeparator = false;
  };

  static std::vector<MenuItem> buildMenuItems(bool hasFootnotes);

  // Menu layout (built dynamically based on whether page has footnotes)
  const std::vector<MenuItem> menuItems;

  int selectedIndex = 0;

  ButtonNavigator buttonNavigator;
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = 0;
  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                                StrId::STR_LANDSCAPE_CCW};
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;

  const std::function<void(uint8_t)> onBack;
  const std::function<void(MenuAction)> onAction;
};
