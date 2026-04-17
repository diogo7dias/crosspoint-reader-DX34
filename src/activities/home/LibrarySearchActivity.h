#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"
#include "util/KeyboardWidget.h"

class LibrarySearchActivity final : public Activity {
 public:
  using OnCompleteCallback = std::function<void(const std::string&)>;
  using OnCancelCallback = std::function<void()>;

  explicit LibrarySearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string folderPath,
                                 const std::vector<std::string>& entries, std::string initialQuery,
                                 OnCompleteCallback onComplete, OnCancelCallback onCancel);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  std::string folderPath;
  const std::vector<std::string>& entries;
  std::string query;
  ButtonNavigator buttonNavigator;
  KeyboardWidget keyboard;
  OnCompleteCallback onComplete;
  OnCancelCallback onCancel;
  std::vector<size_t> previewMatches;

  static constexpr size_t MAX_QUERY_LENGTH = 64;
  static constexpr int MAX_PREVIEW_ROWS = 20;

  void rebuildPreviewMatches();
  void applyKeyResult(const KeyboardWidget::KeyResult& result);
};
