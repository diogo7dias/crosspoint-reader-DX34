#include "CustomFontsSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <string>

#include "MappedInputManager.h"
#include "activities/util/ConfirmDialogActivity.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "fonts/CustomBinFontManager.h"

namespace {
constexpr int kActionDeleteSize = 0;
constexpr int kActionDeleteFamily = 1;
constexpr int kActionCancel = 2;
constexpr int kActionCount = 3;
}  // namespace

void CustomFontsSettingsActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  rebuildRows();
  selectedIndex = rows.empty() ? 0 : std::min<int>(selectedIndex, static_cast<int>(rows.size()) - 1);
  inActionMenu = false;
  actionIndex = 0;
  requestUpdate();
}

void CustomFontsSettingsActivity::onExit() { ActivityWithSubactivity::onExit(); }

void CustomFontsSettingsActivity::rebuildRows() {
  rows.clear();
  const auto& mgr = crosspoint::fonts::CustomBinFontManager::instance();
  // Flatten (family, size) pairs into one row each so the existing list UI
  // and the per-size/per-family delete actions map cleanly.
  for (const auto& fam : mgr.families()) {
    for (const auto& sz : fam.sizes) {
      Row r;
      r.fontName = fam.name;
      r.sizePt = sz.sizePt;
      r.hasRegular = sz.hasRegular;
      r.hasBold = sz.hasBold;
      r.hasItalic = sz.hasItalic;
      r.hasBoldItalic = sz.hasBoldItalic;
      rows.push_back(std::move(r));
    }
  }
}

void CustomFontsSettingsActivity::openActionMenu() {
  if (rows.empty()) return;
  inActionMenu = true;
  actionIndex = kActionDeleteSize;
  requestUpdate();
}

void CustomFontsSettingsActivity::closeActionMenu() {
  inActionMenu = false;
  actionIndex = 0;
  requestUpdate();
}

void CustomFontsSettingsActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (inActionMenu) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      closeActionMenu();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (actionIndex == kActionDeleteSize) {
        runDeleteSize();
      } else if (actionIndex == kActionDeleteFamily) {
        runDeleteFamily();
      } else {
        closeActionMenu();
      }
      return;
    }
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down, MappedInputManager::Button::Right}, [this] {
      actionIndex = (actionIndex + 1) % kActionCount;
      requestUpdate();
    });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up, MappedInputManager::Button::Left}, [this] {
      actionIndex = (actionIndex + kActionCount - 1) % kActionCount;
      requestUpdate();
    });
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (!rows.empty() && mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    openActionMenu();
    return;
  }

  if (rows.empty()) return;

  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % static_cast<int>(rows.size());
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + static_cast<int>(rows.size()) - 1) % static_cast<int>(rows.size());
    requestUpdate();
  });
}

void CustomFontsSettingsActivity::runDeleteSize() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(rows.size())) return;
  const std::string name = rows[selectedIndex].fontName;
  const uint16_t sizePt = rows[selectedIndex].sizePt;
  char header[96];
  snprintf(header, sizeof(header), "%s  %upt", name.c_str(), static_cast<unsigned>(sizePt));
  const std::string prompt = std::string(tr(STR_DELETE_CUSTOM_FONT_SIZE_CONFIRM)) + "\n\n" + header;
  inActionMenu = false;
  exitActivity();
  enterNewActivity(new ConfirmDialogActivity(
      renderer, mappedInput, prompt,
      [this, name, sizePt]() {
        const size_t removed = crosspoint::fonts::CustomBinFontManager::instance().deleteFamilySize(name, sizePt);
        LOG_INF("CFONT", "UI deleted size %s %upt (%u files)", name.c_str(), static_cast<unsigned>(sizePt),
                static_cast<unsigned>(removed));
        exitActivity();
        rebuildRows();
        if (selectedIndex >= static_cast<int>(rows.size())) {
          selectedIndex = rows.empty() ? 0 : static_cast<int>(rows.size()) - 1;
        }
        requestUpdate();
      },
      [this]() {
        exitActivity();
        requestUpdate();
      }));
}

void CustomFontsSettingsActivity::runDeleteFamily() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(rows.size())) return;
  const std::string name = rows[selectedIndex].fontName;
  const std::string prompt = std::string(tr(STR_DELETE_CUSTOM_FONT_FAMILY_CONFIRM)) + "\n\n" + name;
  inActionMenu = false;
  exitActivity();
  enterNewActivity(new ConfirmDialogActivity(
      renderer, mappedInput, prompt,
      [this, name]() {
        const size_t removed = crosspoint::fonts::CustomBinFontManager::instance().deleteFamily(name);
        LOG_INF("CFONT", "UI deleted family %s (%u files)", name.c_str(), static_cast<unsigned>(removed));
        exitActivity();
        rebuildRows();
        if (selectedIndex >= static_cast<int>(rows.size())) {
          selectedIndex = rows.empty() ? 0 : static_cast<int>(rows.size()) - 1;
        }
        requestUpdate();
      },
      [this]() {
        exitActivity();
        requestUpdate();
      }));
}

void CustomFontsSettingsActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_MANAGE_CUSTOM_FONTS), true, EpdFontFamily::REGULAR);

  if (inActionMenu && !rows.empty()) {
    // Show which row the destructive action will hit.
    char header[96];
    snprintf(header, sizeof(header), "%s  %upt", rows[selectedIndex].fontName.c_str(),
             static_cast<unsigned>(rows[selectedIndex].sizePt));
    renderer.drawCenteredText(UI_10_FONT_ID, 40, header, true, EpdFontFamily::BOLD);

    const char* actionLabels[kActionCount] = {
        tr(STR_DELETE_CUSTOM_FONT_SIZE_ACTION),
        tr(STR_DELETE_CUSTOM_FONT_FAMILY_ACTION),
        tr(STR_CANCEL),
    };
    const int rowHeight = 30;
    const int firstRowY = 80;
    for (int i = 0; i < kActionCount; ++i) {
      const int y = firstRowY + i * rowHeight;
      const bool isSelected = (i == actionIndex);
      if (isSelected) {
        renderer.fillRect(0, y - 2, pageWidth - 1, rowHeight);
      }
      renderer.drawCenteredText(UI_10_FONT_ID, y, actionLabels[i], !isSelected);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (rows.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_CUSTOM_FONTS_EMPTY), true, EpdFontFamily::REGULAR);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int rowHeight = 30;
  const int firstRowY = 60;
  // Reserve space for header + button-hint footer (~30 px each). Cap how
  // many rows fit the screen; if there are more rows than fit, page
  // through them by anchoring on the selected row so the cursor stays
  // visible. Handles the full 50-family worst case.
  const int footerMargin = 40;
  const int visibleRows = std::max(1, (pageHeight - firstRowY - footerMargin) / rowHeight);
  const int totalRows = static_cast<int>(rows.size());
  const int pageStartIndex = (selectedIndex / visibleRows) * visibleRows;
  const int pageEndIndex = std::min(totalRows, pageStartIndex + visibleRows);
  for (int i = pageStartIndex; i < pageEndIndex; ++i) {
    const int y = firstRowY + (i - pageStartIndex) * rowHeight;
    const bool isSelected = (i == selectedIndex);
    if (isSelected) {
      renderer.fillRect(0, y - 2, pageWidth - 1, rowHeight);
    }

    // Left: "<name>  <size>pt"
    const auto& r = rows[i];
    const std::string leftLabel = r.fontName + "  " + std::to_string(r.sizePt) + "pt";
    renderer.drawText(UI_10_FONT_ID, 20, y, leftLabel.c_str(), !isSelected);

    // Right: variant flags. 'R' / 'B' / 'I' / 'Z' letters for installed variants,
    // '-' for missing. Z == BoldItalic.
    char flags[9];
    flags[0] = r.hasRegular ? 'R' : '-';
    flags[1] = ' ';
    flags[2] = r.hasBold ? 'B' : '-';
    flags[3] = ' ';
    flags[4] = r.hasItalic ? 'I' : '-';
    flags[5] = ' ';
    flags[6] = r.hasBoldItalic ? 'Z' : '-';
    flags[7] = '\0';
    const int flagsW = renderer.getTextWidth(UI_10_FONT_ID, flags);
    renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - flagsW, y, flags, !isSelected);
  }

  // Page indicator when list spills across multiple pages.
  if (totalRows > visibleRows) {
    const int curPage = (pageStartIndex / visibleRows) + 1;
    const int numPages = (totalRows + visibleRows - 1) / visibleRows;
    char pageBuf[32];
    snprintf(pageBuf, sizeof(pageBuf), "%d/%d", curPage, numPages);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight - footerMargin - 2, pageBuf, true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DELETE_FILE_ACTION), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
