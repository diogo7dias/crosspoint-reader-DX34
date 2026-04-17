#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

/// Displays saved quotes from a _QUOTES.txt sidecar file.
/// Navigate up/down to browse; long-press Select to delete a quote.
class QuotesViewerActivity final : public ActivityWithSubactivity {
 public:
  struct QuoteEntry {
    std::string chapter;
    std::string text;
  };

  explicit QuotesViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                const std::string& quotesFilePath, const std::function<void()>& onGoBack)
      : ActivityWithSubactivity("QuotesViewer", renderer, mappedInput), filePath(quotesFilePath), onGoBack(onGoBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  std::string filePath;
  std::string bookTitle;
  std::vector<QuoteEntry> quotes;
  int selectorIndex = 0;
  int scrollTopIndex = 0;
  ButtonNavigator buttonNavigator;
  const std::function<void()> onGoBack;

  void loadQuotes();
  bool saveQuotes() const;
  void deleteQuote(int index);
  static std::string deriveBookTitle(const std::string& path);
};
