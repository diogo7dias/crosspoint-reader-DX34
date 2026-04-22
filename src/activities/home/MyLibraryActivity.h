/**
 * @file MyLibraryActivity.h
 * @brief File browser for the SD card book library.
 *
 * Scans directories for supported formats (.epub, .txt, .xtc, .xtg, .xth),
 * displays sorted file listings with folder navigation, and launches the
 * appropriate reader activity. Special handling for the /sleep folder
 * (sleep image management with playlist ordering).
 *
 * File list is capped at 300 entries (1000 for /sleep) to stay within heap.
 */
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "RecentBooksStore.h"
#include "util/ButtonNavigator.h"

class MyLibraryActivity final : public ActivityWithSubactivity {
 private:
  struct MoveBrowseEntry {
    std::string name;
    std::string path;
    bool isParent = false;
    bool isMoveHere = false;
  };

  enum class Mode { BROWSE, BMP_VIEW, FILE_ACTIONS, FILE_MOVE_BROWSER };

  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;
  Mode mode = Mode::BROWSE;
  int fileActionIndex = 0;
  int fileMoveIndex = 0;
  std::string selectedFilePath;
  std::string moveBrowserPath = "/";
  bool messagePopupOpen = false;
  std::string messagePopupText;
  std::vector<MoveBrowseEntry> moveBrowseEntries;
  HalDisplay::RefreshMode nextRefreshMode = HalDisplay::FAST_REFRESH;
  // True when FILE_ACTIONS was entered from BMP_VIEW (tap Actions in viewer).
  // Controls menu layout (no "Open Image") and back-destination (returns to viewer).
  bool actionsOpenedFromViewer = false;
  // False until the initial BW + grayscale render of the current image has
  // completed. While false, renderBmpView hides the bottom button hints so
  // the user doesn't try to press buttons that aren't yet responsive.
  bool bmpViewFullyLoaded = false;

  // Files state
  std::string basepath = "/";
  std::vector<std::string> files;
  std::vector<size_t> filteredFileIndexes;
  std::string activeSearchQuery;
  std::string pendingSearchQuery;
  bool pendingSearchSubmit = false;
  bool pendingSearchCancel = false;
  std::string pendingRenameBase;
  bool pendingRenameSubmit = false;
  bool pendingRenameCancel = false;
  std::unordered_map<std::string, std::string> progressPrefixCache;
  size_t fileLoadLimit = 200;
  bool hasMoreFiles = false;
  bool folderHasBooks = false;

  // Callbacks
  const std::function<void(const std::string& path)> onSelectBook;
  const std::function<void()> onGoHome;

  // Data loading
  void loadFiles();
  void loadFilesWithLimit();
  void loadMoreFiles();
  void openSearchActivity();
  void clearSearch();
  void setSearchQuery(const std::string& query);
  void rebuildFilteredFileIndexes();
  bool hasActiveSearch() const;
  size_t entryListOffset() const;
  size_t visibleEntryCount() const;
  size_t totalListCount() const;
  bool isSearchActionRow(size_t listIndex) const;
  bool isClearSearchRow(size_t listIndex) const;
  bool isLoadMoreRow(size_t listIndex) const;
  std::optional<size_t> rawFileIndexForListIndex(size_t listIndex) const;
  std::optional<size_t> rawFileIndexForPath(const std::string& path) const;
  size_t listIndexForRawFileIndex(size_t rawIndex) const;
  void clampSelectorIndex();
  std::string getDisplayNameForRawFile(size_t rawIndex);
  std::string getRowTextForListIndex(size_t listIndex);
  std::string makeAbsolutePath(const std::string& name) const;
  static std::string getBasename(const std::string& path);
  static bool isBookFile(const std::string& filename);
  static bool isBmpFile(const std::string& filename);
  static bool isManagedFile(const std::string& filename);
  void enterBmpView(const std::string& bmpPath);
  void enterFileActions(const std::string& filePath);
  void enterFileMoveBrowser();
  void openKeyboardForRenameBmp();
  void renameSelectedBmp(const std::string& newBase);
  void loadMoveBrowseEntries();
  int getFileActionCount() const;
  std::string getFileActionLabel(int index) const;
  void showMessagePopup(const std::string& message);
  bool copyFile(const std::string& srcPath, const std::string& dstPath) const;
  bool moveSelectedFileTo(const std::string& targetDir, std::string* destinationPath = nullptr) const;
  bool deleteFile(const std::string& path);
  bool deleteSelectedFile();
  void requestCleanRefresh();
  void loopSubActivity();
  void loopMessagePopup();
  void loopBmpView();
  void loopFileActions();
  void loopFileMoveBrowser();
  void loopBrowse();
  void displayFrame();
  void renderBmpView();
  void renderFileActions();
  void renderFileMoveBrowser();
  size_t findEntry(const std::string& name) const;

 public:
  explicit MyLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             const std::function<void()>& onGoHome,
                             const std::function<void(const std::string& path)>& onSelectBook,
                             std::string initialPath = "/")
      : ActivityWithSubactivity("MyLibrary", renderer, mappedInput),
        basepath(initialPath.empty() ? "/" : std::move(initialPath)),
        onSelectBook(onSelectBook),
        onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;
};
