#include "QuotesViewerActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "ReaderLayoutSafety.h"
#include "activities/util/ConfirmDialogActivity.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long kHoldDeleteMs = 2000;
constexpr int kLineHeight = 22;
constexpr int kQuoteGap = 8;  // vertical gap between quotes
constexpr int kButtonHintsReserve = 50;
constexpr size_t kMaxFileRead = 65536;
}  // namespace

// ── Parsing ─────────────────────────────────────────────────────────────────

void QuotesViewerActivity::loadQuotes() {
  quotes.clear();
  FsFile file;
  if (!Storage.openFileForRead("QV", filePath, file)) return;

  const size_t fileSize = file.size();
  const size_t readSize = (fileSize < kMaxFileRead) ? fileSize : kMaxFileRead;
  if (fileSize > kMaxFileRead) {
    LOG_INF("QV", "Quotes file %s is %u bytes, truncating to %u",
            filePath.c_str(), (unsigned)fileSize, (unsigned)kMaxFileRead);
  }
  std::string buf(readSize, '\0');
  file.read(&buf[0], readSize);
  file.close();

  // Format:  [Chapter Title]\nquote text\n---\n\n
  size_t pos = 0;
  while (pos < buf.size()) {
    while (pos < buf.size() &&
           (buf[pos] == '\n' || buf[pos] == '\r' || buf[pos] == ' '))
      ++pos;
    if (pos >= buf.size()) break;

    QuoteEntry entry;

    // Optional [Chapter] header
    if (buf[pos] == '[') {
      auto close = buf.find(']', pos);
      if (close != std::string::npos) {
        entry.chapter = buf.substr(pos + 1, close - pos - 1);
        pos = close + 1;
        while (pos < buf.size() && (buf[pos] == '\n' || buf[pos] == '\r'))
          ++pos;
      }
    }

    // Quote text until --- separator
    auto sep = buf.find("\n---", pos);
    if (sep == std::string::npos) {
      entry.text = buf.substr(pos);
    } else {
      entry.text = buf.substr(pos, sep - pos);
      pos = sep + 4;
    }

    // Trim trailing whitespace
    while (!entry.text.empty() &&
           (entry.text.back() == '\n' || entry.text.back() == '\r' ||
            entry.text.back() == ' '))
      entry.text.pop_back();

    if (!entry.text.empty()) quotes.push_back(std::move(entry));
    if (sep == std::string::npos) break;
  }
}

bool QuotesViewerActivity::saveQuotes() const {
  if (quotes.empty()) {
    // All quotes deleted — remove the file
    Storage.remove(filePath.c_str());
    LOG_INF("QV", "All quotes deleted, removed %s", filePath.c_str());
    return true;
  }

  FsFile file = Storage.open(filePath.c_str(), O_WRITE | O_CREAT | O_TRUNC);
  if (!file) {
    LOG_ERR("QV", "Failed to open quotes file for writing: %s",
            filePath.c_str());
    return false;
  }

  for (size_t i = 0; i < quotes.size(); i++) {
    const auto& q = quotes[i];
    if (!q.chapter.empty()) {
      file.print("[");
      file.print(q.chapter.c_str());
      file.println("]");
    }
    file.println(q.text.c_str());
    file.println("---");
    if (i + 1 < quotes.size()) {
      file.println();
    }
  }

  file.close();
  LOG_DBG("QV", "Saved %d quotes to %s", static_cast<int>(quotes.size()),
          filePath.c_str());
  return true;
}

void QuotesViewerActivity::deleteQuote(int index) {
  if (index < 0 || index >= static_cast<int>(quotes.size())) return;
  quotes.erase(quotes.begin() + index);
  saveQuotes();
  if (selectorIndex >= static_cast<int>(quotes.size()) && selectorIndex > 0)
    selectorIndex--;
}

// ── Helpers ─────────────────────────────────────────────────────────────────

std::string QuotesViewerActivity::deriveBookTitle(const std::string& path) {
  auto slash = path.rfind('/');
  std::string filename =
      (slash != std::string::npos) ? path.substr(slash + 1) : path;
  const std::string suffix = "_QUOTES.txt";
  if (filename.size() > suffix.size() &&
      filename.compare(filename.size() - suffix.size(), suffix.size(),
                       suffix) == 0) {
    filename = filename.substr(0, filename.size() - suffix.size());
  }
  return filename;
}

// Build the display string for a quote (chapter prefix + text, newlines → spaces).
static std::string buildQuoteText(const QuotesViewerActivity::QuoteEntry& entry) {
  std::string text;
  if (!entry.chapter.empty()) {
    text = "[" + entry.chapter + "] ";
  }
  text += entry.text;
  std::replace(text.begin(), text.end(), '\n', ' ');
  std::replace(text.begin(), text.end(), '\r', ' ');
  return text;
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

void QuotesViewerActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  bookTitle = deriveBookTitle(filePath);
  loadQuotes();
  selectorIndex = 0;
  requestUpdate();
}

void QuotesViewerActivity::onExit() { ActivityWithSubactivity::onExit(); }

// ── Input ───────────────────────────────────────────────────────────────────

void QuotesViewerActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  const int totalItems = static_cast<int>(quotes.size());

  if (totalItems == 0) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoBack();
    }
    return;
  }

  // Long-press Select: delete quote
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= kHoldDeleteMs && selectorIndex >= 0 &&
      selectorIndex < totalItems) {
    mappedInput.suppressUntilAllReleased();
    const int deleteIdx = selectorIndex;
    // Show first ~40 chars of the quote in the confirmation
    std::string preview = quotes[deleteIdx].text;
    std::replace(preview.begin(), preview.end(), '\n', ' ');
    if (preview.size() > 40) {
      preview.resize(37);
      preview += "...";
    }
    const std::string msg = "Delete quote?\n" + preview;
    enterNewActivity(new ConfirmDialogActivity(
        renderer, mappedInput, msg,
        [this, deleteIdx] {
          deleteQuote(deleteIdx);
          exitActivity();
          requestUpdate();
        },
        [this] {
          exitActivity();
          requestUpdate();
        }));
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
    return;
  }

  buttonNavigator.onNext([this, totalItems] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, totalItems] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
    requestUpdate();
  });
}

// ── Render ──────────────────────────────────────────────────────────────────

void QuotesViewerActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  auto metrics = BaseMetrics::values;

  // Header — book title
  const int titleX =
      (pageWidth -
       renderer.getTextWidth(UI_12_FONT_ID, bookTitle.c_str(),
                             EpdFontFamily::REGULAR)) /
      2;
  renderer.drawText(UI_12_FONT_ID, std::max(4, titleX),
                    metrics.topPadding + 5, bookTitle.c_str(), true,
                    EpdFontFamily::REGULAR);

  // Count indicator
  const int countY = metrics.topPadding + 5 + 30;
  const std::string countStr = std::to_string(quotes.size()) + " quotes";
  renderer.drawCenteredText(UI_10_FONT_ID, countY, countStr.c_str());

  const int totalItems = static_cast<int>(quotes.size());

  if (totalItems == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, screenHeight / 2, "No quotes");
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3,
                        labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int listStartY = countY + 25;
  const int listEndY = screenHeight - kButtonHintsReserve;
  const int textMaxWidth = pageWidth - 40;  // 20px padding each side

  // Ensure scrollTopIndex keeps selectorIndex visible.
  if (selectorIndex < scrollTopIndex) {
    scrollTopIndex = selectorIndex;
  }

  // Forward pass: if selectorIndex is beyond visible area, advance scrollTopIndex.
  // We do this by rendering from scrollTopIndex and checking if selectorIndex fits.
  bool selectorVisible = false;
  while (!selectorVisible && scrollTopIndex < totalItems) {
    int y = listStartY;
    for (int i = scrollTopIndex; i < totalItems && y < listEndY; i++) {
      const auto text = buildQuoteText(quotes[i]);
      const auto lines = ReaderLayoutSafety::wrapText(renderer, UI_10_FONT_ID, text, textMaxWidth);
      const int entryHeight = static_cast<int>(lines.size()) * kLineHeight + kQuoteGap;
      if (i == selectorIndex) {
        // Check if the entire quote fits; if not, scroll further
        if (y + entryHeight - kQuoteGap <= listEndY) {
          selectorVisible = true;
        }
        break;
      }
      y += entryHeight;
    }
    if (!selectorVisible) scrollTopIndex++;
  }

  // Render visible quotes
  int currentY = listStartY;
  for (int i = scrollTopIndex; i < totalItems && currentY < listEndY; i++) {
    const auto text = buildQuoteText(quotes[i]);
    const auto lines = ReaderLayoutSafety::wrapText(renderer, UI_10_FONT_ID, text, textMaxWidth);
    const int textHeight = static_cast<int>(lines.size()) * kLineHeight;
    const bool isSelected = (i == selectorIndex);

    // Don't start a quote if not even the first line fits
    if (currentY + kLineHeight > listEndY) break;

    if (isSelected) {
      renderer.fillRect(0, currentY - 2, pageWidth - 1, std::min(textHeight + 4, listEndY - currentY + 2));
    }

    for (size_t ln = 0; ln < lines.size() && currentY < listEndY; ln++) {
      renderer.drawText(UI_10_FONT_ID, 20, currentY, lines[ln].c_str(), !isSelected);
      currentY += kLineHeight;
    }
    currentY += kQuoteGap;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Hold:Del",
                                            tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3,
                      labels.btn4);

  renderer.displayBuffer();
}
