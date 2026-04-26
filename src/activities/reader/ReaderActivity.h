#pragma once
#include <memory>

#include "../ActivityWithSubactivity.h"
#include "activities/home/MyLibraryActivity.h"

class Epub;
class Xtc;
class Txt;

class ReaderActivity final : public ActivityWithSubactivity {
  std::string initialBookPath;
  std::string currentBookPath;  // Track current book path for navigation
  const std::function<void()> onGoBack;
  const std::function<void(const std::string&)> onGoToLibrary;
  std::unique_ptr<Epub> loadEpub(const std::string& path);
  std::unique_ptr<Xtc> loadXtc(const std::string& path);
  std::unique_ptr<Txt> loadTxt(const std::string& path);
  static bool isXtcFile(const std::string& path);
  static bool isTxtFile(const std::string& path);
  static bool isQuotesFile(const std::string& path);

  static std::string extractFolderPath(const std::string& filePath);
  void goToLibrary(const std::string& fromBookPath = "");
  void openBookPath(const std::string& bookPath);
  void onGoToEpubReader(std::unique_ptr<Epub> epub);
  void onGoToXtcReader(std::unique_ptr<Xtc> xtc);
  void onGoToTxtReader(std::unique_ptr<Txt> txt);

 public:
  explicit ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath,
                          const std::function<void()>& onGoBack,
                          const std::function<void(const std::string&)>& onGoToLibrary)
      : ActivityWithSubactivity("Reader", renderer, mappedInput),
        initialBookPath(std::move(initialBookPath)),
        onGoBack(onGoBack),
        onGoToLibrary(onGoToLibrary) {}
  void onEnter() override;
  bool isReaderActivity() const override { return true; }

 protected:
  // Override: when an inner reader subactivity (Epub/Xtc/Txt/Quotes) cannot
  // bring itself up and is replaced by the OOM screen, the user dismissing
  // that screen needs to land somewhere navigable. ReaderActivity has no
  // chrome of its own, so dropping back to the library at the failing
  // book's folder is the right escape hatch.
  void onSubactivityEntryFailedFatally() override {
    exitActivity();
    goToLibrary(currentBookPath);
  }
};
