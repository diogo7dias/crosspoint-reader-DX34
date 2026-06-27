#include "EnumOptionPicker.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "fontIds.h"

namespace crosspoint::settings {

void EnumOptionPicker::open(StrId title, std::vector<std::string> labels, int currentIndex) {
  title_ = title;
  labels_ = std::move(labels);
  selected_ = labels_.empty() ? 0 : std::max(0, std::min(currentIndex, static_cast<int>(labels_.size()) - 1));
  open_ = true;
}

void EnumOptionPicker::moveUp() {
  if (labels_.empty()) {
    return;
  }
  selected_ = (selected_ == 0) ? static_cast<int>(labels_.size()) - 1 : selected_ - 1;
}

void EnumOptionPicker::moveDown() {
  if (labels_.empty()) {
    return;
  }
  selected_ = (selected_ + 1) % static_cast<int>(labels_.size());
}

void EnumOptionPicker::render(GfxRenderer& renderer, int pageWidth, int pageHeight) const {
  if (labels_.empty()) {
    return;
  }

  const int titleFont = UI_12_FONT_ID;
  const int rowFont = UI_10_FONT_ID;
  // tr() is a literal-token macro; the title StrId is only known at runtime, so
  // resolve it through the instance API instead.
  const std::string titleStr = I18N.get(title_);
  const char* title = titleStr.c_str();

  constexpr int kPad = 16;      // padding inside the popup frame
  constexpr int kRowPadV = 4;   // vertical padding inside each row
  constexpr int kTitleH = 18;   // reserved height for the title line
  constexpr int kTitleGap = 8;  // gap between title and first row
  constexpr int kScreenInset = 20;
  constexpr int kVerticalInset = 40;

  const int rowTextH = renderer.getTextHeight(rowFont);
  const int rowH = rowTextH + kRowPadV * 2;

  // Width: widest label (or title), clamped to the screen.
  int maxLabelW = renderer.getTextWidth(titleFont, title);
  for (const auto& label : labels_) {
    maxLabelW = std::max(maxLabelW, renderer.getTextWidth(rowFont, label.c_str()));
  }
  const int popupW = std::min(pageWidth - kScreenInset, maxLabelW + kPad * 2);

  // Height: as many rows as fit, capped to the row count.
  const int chromeH = kPad + kTitleH + kTitleGap + kPad;
  const int maxRowsArea = (pageHeight - kVerticalInset) - chromeH;
  int visibleRows = std::max(1, maxRowsArea / rowH);
  visibleRows = std::min(visibleRows, static_cast<int>(labels_.size()));
  const int popupH = chromeH + visibleRows * rowH;
  const int popupX = (pageWidth - popupW) / 2;
  const int popupY = (pageHeight - popupH) / 2;

  // Scroll the viewport so the selected row stays visible.
  int firstVisible = 0;
  if (selected_ >= visibleRows) {
    firstVisible = selected_ - visibleRows + 1;
  }
  firstVisible = std::min(firstVisible, std::max(0, static_cast<int>(labels_.size()) - visibleRows));

  // Frame: black border, white fill.
  renderer.fillRect(popupX - 2, popupY - 2, popupW + 4, popupH + 4, true);
  renderer.fillRect(popupX, popupY, popupW, popupH, false);

  const int titleW = renderer.getTextWidth(titleFont, title);
  renderer.drawText(titleFont, popupX + (popupW - titleW) / 2, popupY + kPad - 4, title, true);

  int rowY = popupY + kPad + kTitleH + kTitleGap;
  const int rowX = popupX + kPad;
  for (int v = 0; v < visibleRows; ++v) {
    const int i = firstVisible + v;
    if (i >= static_cast<int>(labels_.size())) {
      break;
    }
    const bool isSelected = (i == selected_);
    if (isSelected) {
      renderer.fillRect(popupX + 4, rowY, popupW - 8, rowH, true);
    }
    renderer.drawText(rowFont, rowX, rowY + kRowPadV, labels_[i].c_str(), !isSelected);
    rowY += rowH;
  }
}

}  // namespace crosspoint::settings
