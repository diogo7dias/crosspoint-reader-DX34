#include "MyLibraryActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <cmath>
#include <esp_task_wdt.h>
#include <random>

#include "util/TransitionFeedback.h"

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Paths.h"
#include "LibrarySearchActivity.h"
#include "LibrarySearchSupport.h"
#include "MappedInputManager.h"
#include "activities/boot_sleep/SleepActivity.h"
#include "activities/util/ConfirmDialogActivity.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "util/BookProgress.h"
#include "util/FavoriteBmp.h"
#include "activities/network/QRShareActivity.h"
#include "util/StatusPopup.h"
#include "util/StringUtils.h"

void sortFileList(std::vector<std::string>& strs);

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr size_t kDefaultFileLimit = 200;
constexpr size_t kFilesPerBatch = 200;
constexpr size_t kFileLoadProgressThreshold = 50;
constexpr size_t kFileLoadProgressInterval = 100;

std::string rtrimSpaces(std::string text) {
  while (!text.empty() && text.back() == ' ') {
    text.pop_back();
  }
  return text;
}

std::string formatLibraryProgressPrefix(const std::optional<int>& percent) {
  if (!percent.has_value() || percent.value() <= 1) {
    return "-";
  }
  return "[" + std::to_string(percent.value()) + "%]";
}

bool equalsIgnoreCaseAscii(const std::string& value, const char* target) {
  size_t i = 0;
  for (; i < value.size() && target[i] != '\0'; ++i) {
    char lhs = value[i];
    char rhs = target[i];
    if (lhs >= 'A' && lhs <= 'Z') lhs = static_cast<char>(lhs - 'A' + 'a');
    if (rhs >= 'A' && rhs <= 'Z') rhs = static_cast<char>(rhs - 'A' + 'a');
    if (lhs != rhs) return false;
  }
  return i == value.size() && target[i] == '\0';
}

bool shouldCollapseMoveFolder(const std::string& folderName) {
  return equalsIgnoreCaseAscii(folderName, "book") ||
         equalsIgnoreCaseAscii(folderName, "books");
}

std::string joinPath(const std::string& parent, const std::string& child) {
  if (parent.empty() || parent == "/") {
    return "/" + child;
  }
  return parent + "/" + child;
}

// maxDepth caps recursion to prevent stack overflow on deeply nested trees.
// Each level uses ~512 bytes of stack (name[256] + locals + childDirs vector);
// 5 levels ≈ 2.5 KB which is safe on the ESP32-C3's ~15 KB stack.
constexpr int kMaxMoveDestDepth = 5;

void collectMoveDestinationPaths(const std::string& basePath,
                                 std::vector<std::string>& outPaths,
                                 int depth = 0) {
  if (depth >= kMaxMoveDestDepth) return;

  auto dir = Storage.open(basePath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  std::vector<std::string> childDirs;
  char name[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    if ((!SETTINGS.showHiddenFiles && name[0] == '.') || strcmp(name, "System Volume Information") == 0) {
      file.close();
      continue;
    }
    if (file.isDirectory()) {
      childDirs.emplace_back(std::string(name) + "/");
    }
    file.close();
    esp_task_wdt_reset();
  }
  dir.close();

  sortFileList(childDirs);

  for (const auto& childDir : childDirs) {
    const std::string folderName = childDir.substr(0, childDir.size() - 1);
    const std::string fullPath = joinPath(basePath, folderName);
    outPaths.push_back(fullPath);
    if (!shouldCollapseMoveFolder(folderName)) {
      collectMoveDestinationPaths(fullPath, outPaths, depth + 1);
    }
  }
}

std::vector<std::string> wrapTextToWidth(const GfxRenderer& renderer, const int fontId, const std::string& text,
                                         const int maxWidth) {
  if (text.empty()) return {""};

  std::vector<std::string> lines;
  size_t pos = 0;

  while (pos < text.size()) {
    while (pos < text.size() && text[pos] == ' ') pos++;
    if (pos >= text.size()) break;

    size_t end = pos;
    size_t lastSpace = std::string::npos;

    while (end < text.size()) {
      if (text[end] == ' ') lastSpace = end;
      const std::string candidate = text.substr(pos, end - pos + 1);
      if (renderer.getTextWidth(fontId, candidate.c_str()) > maxWidth) break;
      end++;
    }

    if (end >= text.size()) {
      lines.push_back(rtrimSpaces(text.substr(pos)));
      break;
    }

    if (end == pos) {
      // Force at least one character so very long unbroken tokens still wrap.
      size_t forcedEnd = pos + 1;
      while (forcedEnd < text.size()) {
        const std::string forcedCandidate = text.substr(pos, forcedEnd - pos + 1);
        if (renderer.getTextWidth(fontId, forcedCandidate.c_str()) > maxWidth) break;
        forcedEnd++;
      }
      lines.push_back(rtrimSpaces(text.substr(pos, forcedEnd - pos)));
      pos = forcedEnd;
      continue;
    }

    size_t split = (lastSpace != std::string::npos && lastSpace >= pos) ? lastSpace : (end - 1);
    lines.push_back(rtrimSpaces(text.substr(pos, split - pos + 1)));
    pos = split + 1;
  }

  if (lines.empty()) lines.push_back("");
  return lines;
}
}  // namespace

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    // Directories first
    bool isDir1 = str1.back() == '/';
    bool isDir2 = str2.back() == '/';
    if (isDir1 != isDir2) return isDir1;

    // Start naive natural sort
    const char* s1 = str1.c_str();
    const char* s2 = str2.c_str();

    // Iterate while both strings have characters
    while (*s1 && *s2) {
      // Check if both are at the start of a number
      if (isdigit(*s1) && isdigit(*s2)) {
        // Skip leading zeros and track them
        const char* start1 = s1;
        const char* start2 = s2;
        while (*s1 == '0') s1++;
        while (*s2 == '0') s2++;

        // Count digits to compare lengths first
        int len1 = 0, len2 = 0;
        while (isdigit(s1[len1])) len1++;
        while (isdigit(s2[len2])) len2++;

        // Different length so return smaller integer value
        if (len1 != len2) return len1 < len2;

        // Same length so compare digit by digit
        for (int i = 0; i < len1; i++) {
          if (s1[i] != s2[i]) return s1[i] < s2[i];
        }

        // Numbers equal so advance pointers
        s1 += len1;
        s2 += len2;
      } else {
        // Regular case-insensitive character comparison
        char c1 = tolower(*s1);
        char c2 = tolower(*s2);
        if (c1 != c2) return c1 < c2;
        s1++;
        s2++;
      }
    }

    // One string is prefix of other
    return *s1 == '\0' && *s2 != '\0';
  });
}


void MyLibraryActivity::loadFiles() {
  fileLoadLimit = kDefaultFileLimit;
  loadFilesWithLimit();
}

void MyLibraryActivity::loadMoreFiles() {
  fileLoadLimit += kFilesPerBatch;
  const size_t savedSelector = selectorIndex;
  loadFilesWithLimit();
  selectorIndex = savedSelector;
  clampSelectorIndex();
}

void MyLibraryActivity::loadFilesWithLimit() {
  files.clear();
  hasMoreFiles = false;
  progressPrefixCache.clear();

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();
  files.reserve(std::min(fileLoadLimit, static_cast<size_t>(512)));

  bool hasBooks = false;
  char name[256];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if ((!SETTINGS.showHiddenFiles && name[0] == '.') || strcmp(name, "System Volume Information") == 0) {
      file.close();
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(std::string(name) + "/");
    } else {
      auto filename = std::string(name);
      if (isManagedFile(filename)) {
        files.emplace_back(filename);
        if (!hasBooks && isBookFile(filename)) {
          hasBooks = true;
        }
      }
    }
    file.close();
    esp_task_wdt_reset();

    // Only cap non-book folders (image-only folders like sleep library)
    if (!hasBooks && files.size() >= fileLoadLimit) {
      // Check if there are more relevant files in the directory
      for (auto next = root.openNextFile(); next; next = root.openNextFile()) {
        next.getName(name, sizeof(name));
        const bool skip =
            (!SETTINGS.showHiddenFiles && name[0] == '.') || strcmp(name, "System Volume Information") == 0;
        const bool relevant =
            !skip && (next.isDirectory() || isManagedFile(std::string(name)));
        next.close();
        esp_task_wdt_reset();
        if (relevant) {
          hasMoreFiles = true;
          break;
        }
        if (skip) continue;
        break;
      }
      break;
    }
  }
  root.close();
  folderHasBooks = hasBooks;

  sortFileList(files);

  // Randomize display order for media-browsing folders
  bool shouldShuffle = (basepath == "/sleep pause" || basepath == "/sleep library");
  if (basepath == "/books" && SETTINGS.booksFolderOrder == 1) shouldShuffle = true;
  if (shouldShuffle) {
    // Directories stay sorted at top; shuffle only files
    auto firstFile = std::partition_point(files.begin(), files.end(),
        [](const std::string& s) { return !s.empty() && s.back() == '/'; });
    std::shuffle(firstFile, files.end(), std::mt19937{esp_random()});
  }

  rebuildFilteredFileIndexes();
}

void MyLibraryActivity::openSearchActivity() {
  enterNewActivity(new LibrarySearchActivity(
      renderer, mappedInput, basepath, files, activeSearchQuery,
      [this](const std::string& query) {
        pendingSearchQuery = query;
        pendingSearchSubmit = true;
      },
      [this]() {
        pendingSearchCancel = true;
      }));
}

void MyLibraryActivity::clearSearch() {
  activeSearchQuery.clear();
  rebuildFilteredFileIndexes();
  selectorIndex = 0;
}

void MyLibraryActivity::setSearchQuery(const std::string& query) {
  activeSearchQuery = query;
  rebuildFilteredFileIndexes();
  if (activeSearchQuery.empty()) {
    selectorIndex = 0;
  } else if (visibleEntryCount() > 0) {
    selectorIndex = entryListOffset();
  } else {
    selectorIndex = 1;
  }
  clampSelectorIndex();
}

void MyLibraryActivity::rebuildFilteredFileIndexes() {
  if (activeSearchQuery.empty()) {
    filteredFileIndexes.clear();
    return;
  }

  filteredFileIndexes =
      LibrarySearchSupport::rankMatches(files, activeSearchQuery);
}

bool MyLibraryActivity::hasActiveSearch() const {
  return !activeSearchQuery.empty();
}

size_t MyLibraryActivity::entryListOffset() const {
  if (!folderHasBooks) return 0;
  return hasActiveSearch() ? 2 : 1;
}

size_t MyLibraryActivity::visibleEntryCount() const {
  return hasActiveSearch() ? filteredFileIndexes.size() : files.size();
}

size_t MyLibraryActivity::totalListCount() const {
  size_t count = entryListOffset() + visibleEntryCount();
  if (hasMoreFiles && !hasActiveSearch()) {
    count += 1;
  }
  return count;
}

bool MyLibraryActivity::isSearchActionRow(const size_t listIndex) const {
  return folderHasBooks && listIndex == 0;
}

bool MyLibraryActivity::isClearSearchRow(const size_t listIndex) const {
  return hasActiveSearch() && listIndex == 1;
}

bool MyLibraryActivity::isLoadMoreRow(const size_t listIndex) const {
  return hasMoreFiles && !hasActiveSearch() &&
         listIndex == entryListOffset() + visibleEntryCount();
}

std::optional<size_t> MyLibraryActivity::rawFileIndexForListIndex(
    const size_t listIndex) const {
  if (listIndex < entryListOffset()) {
    return std::nullopt;
  }

  const size_t visibleIndex = listIndex - entryListOffset();
  if (hasActiveSearch()) {
    if (visibleIndex >= filteredFileIndexes.size()) {
      return std::nullopt;
    }
    return filteredFileIndexes[visibleIndex];
  }

  if (visibleIndex >= files.size()) {
    return std::nullopt;
  }
  return visibleIndex;
}

std::optional<size_t> MyLibraryActivity::rawFileIndexForPath(
    const std::string& path) const {
  // Compare by basename to avoid N string allocations (OOM on large folders)
  const std::string name = getBasename(path);
  for (size_t i = 0; i < files.size(); ++i) {
    if (files[i] == name) {
      return i;
    }
  }
  return std::nullopt;
}

size_t MyLibraryActivity::listIndexForRawFileIndex(const size_t rawIndex) const {
  if (hasActiveSearch()) {
    for (size_t i = 0; i < filteredFileIndexes.size(); ++i) {
      if (filteredFileIndexes[i] == rawIndex) {
        return entryListOffset() + i;
      }
    }
    return 0;
  }

  if (rawIndex >= files.size()) {
    return 0;
  }
  return entryListOffset() + rawIndex;
}

void MyLibraryActivity::clampSelectorIndex() {
  const size_t listCount = totalListCount();
  if (listCount == 0) {
    selectorIndex = 0;
    return;
  }
  if (selectorIndex >= listCount) {
    selectorIndex = listCount - 1;
  }
}

std::string MyLibraryActivity::makeAbsolutePath(const std::string& name) const {
  std::string fullPath = basepath;
  if (fullPath.empty() || fullPath.back() != '/') {
    fullPath += "/";
  }
  return fullPath + name;
}

std::string MyLibraryActivity::getBasename(const std::string& path) {
  const auto slashPos = path.find_last_of('/');
  return (slashPos == std::string::npos) ? path : path.substr(slashPos + 1);
}

bool MyLibraryActivity::isBookFile(const std::string& filename) {
  return StringUtils::checkFileExtension(filename, ".epub") || StringUtils::checkFileExtension(filename, ".xtch") ||
         StringUtils::checkFileExtension(filename, ".xtc") || StringUtils::checkFileExtension(filename, ".txt") ||
         StringUtils::checkFileExtension(filename, ".md");
}

bool MyLibraryActivity::isBmpFile(const std::string& filename) {
  return StringUtils::checkFileExtension(filename, ".bmp");
}

bool MyLibraryActivity::isManagedFile(const std::string& filename) { return isBookFile(filename) || isBmpFile(filename); }

std::string MyLibraryActivity::getDisplayNameForRawFile(const size_t rawIndex) {
  if (rawIndex >= files.size()) {
    return "";
  }

  const std::string& name = files[rawIndex];
  if (!name.empty() && name.back() == '/') {
    return "[" + name.substr(0, name.size() - 1) + "]";
  }

  if (isBmpFile(name)) {
    return FavoriteBmp::displayNameForPath(makeAbsolutePath(name));
  }

  if (!isBookFile(name)) {
    return name;
  }
  return name;
}

std::string MyLibraryActivity::getRowTextForListIndex(const size_t listIndex) {
  if (isSearchActionRow(listIndex)) {
    if (activeSearchQuery.empty()) {
      return "Search current folder";
    }
    return "Edit search: " + activeSearchQuery;
  }

  if (isClearSearchRow(listIndex)) {
    return "Clear search";
  }

  if (isLoadMoreRow(listIndex)) {
    return "Load more files...";
  }

  const auto rawIndex = rawFileIndexForListIndex(listIndex);
  if (!rawIndex.has_value()) {
    return "";
  }

  const std::string& name = files[*rawIndex];
  std::string rowText = getDisplayNameForRawFile(*rawIndex);
  if (!name.empty() && name.back() != '/' && isBookFile(name)) {
    const std::string fullPath = makeAbsolutePath(name);
    auto cached = progressPrefixCache.find(fullPath);
    if (cached == progressPrefixCache.end()) {
      cached = progressPrefixCache
                   .emplace(fullPath, formatLibraryProgressPrefix(
                                          BookProgress::getPercent(fullPath)))
                   .first;
    }
    rowText = cached->second + "  " + rowText;
  }

  return rowText;
}

void MyLibraryActivity::enterBmpView(const std::string& bmpPath) {
  selectedFilePath = bmpPath;
  mode = Mode::BMP_VIEW;
  requestCleanRefresh();
  requestUpdate();
}

void MyLibraryActivity::enterFileActions(const std::string& filePath) {
  selectedFilePath = filePath;
  fileActionIndex = 0;
  mode = Mode::FILE_ACTIONS;
  requestCleanRefresh();
  requestUpdate();
}

void MyLibraryActivity::enterFileMoveBrowser() {
  moveBrowserPath = "/";
  fileMoveIndex = 0;
  loadMoveBrowseEntries();
  mode = Mode::FILE_MOVE_BROWSER;
}

int MyLibraryActivity::getFileActionCount() const {
  return isBmpFile(selectedFilePath) ? 7 : 5;
}

std::string MyLibraryActivity::getFileActionLabel(const int index) const {
  if (isBmpFile(selectedFilePath)) {
    switch (index) {
      case 0:
        return "Open Image";
      case 1:
        return "Move File";
      case 2:
        return "Move to Sleep";
      case 3:
        return FavoriteBmp::isFavoritePath(selectedFilePath) ? "Unfavorite" : "Favorite";
      case 4:
        return tr(STR_DOWNLOAD_IMAGE_VIA_QR);
      case 5:
        return "Delete File";
      default:
        return "Cancel";
    }
  }

  switch (index) {
    case 0:
      return "Open Book";
    case 1:
      return "Move File";
    case 2:
      return tr(STR_DOWNLOAD_BOOK_VIA_QR);
    case 3:
      return "Delete File";
    default:
      return "Cancel";
  }
}

void MyLibraryActivity::showMessagePopup(const std::string& message) {
  if (message.empty()) {
    return;
  }
  messagePopupText = message;
  messagePopupOpen = true;
  requestUpdate();
}

void MyLibraryActivity::loadMoveBrowseEntries() {
  moveBrowseEntries.clear();
  moveBrowseEntries.push_back({"[ROOT] /", "/", false, false});

  std::vector<std::string> destinationPaths;
  collectMoveDestinationPaths("/", destinationPaths);
  std::transform(destinationPaths.begin(), destinationPaths.end(),
                 std::back_inserter(moveBrowseEntries),
                 [](const std::string& path) {
                   return MoveBrowseEntry{path, path, false, false};
                 });

  if (fileMoveIndex >= static_cast<int>(moveBrowseEntries.size())) {
    fileMoveIndex = std::max(0, static_cast<int>(moveBrowseEntries.size()) - 1);
  }
}

bool MyLibraryActivity::copyFile(const std::string& srcPath, const std::string& dstPath) const {
  FsFile src;
  if (!Storage.openFileForRead("LIB", srcPath, src)) {
    return false;
  }

  FsFile dst;
  if (!Storage.openFileForWrite("LIB", dstPath, dst)) {
    src.close();
    return false;
  }

  uint8_t buffer[1024];
  while (src.available()) {
    esp_task_wdt_reset();
    const auto bytesRead = src.read(buffer, sizeof(buffer));
    if (bytesRead == 0) break;
    if (dst.write(buffer, bytesRead) != bytesRead) {
      src.close();
      dst.close();
      return false;
    }
  }

  src.close();
  dst.close();
  return true;
}

bool MyLibraryActivity::moveSelectedFileTo(const std::string& targetDir,
                                           std::string* destinationPath) const {
  if (selectedFilePath.empty()) return false;

  std::string normalizedTarget = targetDir;
  if (normalizedTarget.empty()) normalizedTarget = "/";
  if (normalizedTarget.back() != '/') normalizedTarget += "/";
  if (normalizedTarget == "/sleep/" &&
      !FavoriteBmp::canPlacePathInSleep(selectedFilePath)) {
    return false;
  }

  const std::string filename = getBasename(selectedFilePath);
  const std::string destination = normalizedTarget + filename;

  if (destination == selectedFilePath || Storage.exists(destination.c_str())) {
    return false;
  }

  // rename is instant on the same filesystem (SD card FAT32).
  // Fall back to copy+delete only if rename fails (shouldn't happen).
  if (!Storage.rename(selectedFilePath.c_str(), destination.c_str())) {
    if (!copyFile(selectedFilePath, destination)) {
      Storage.remove(destination.c_str());
      return false;
    }
    if (!Storage.remove(selectedFilePath.c_str())) {
      Storage.remove(destination.c_str());
      return false;
    }
  }

  if (isBookFile(selectedFilePath)) {
    RECENT_BOOKS.removeBook(selectedFilePath);
  } else if (isBmpFile(selectedFilePath)) {
    FavoriteBmp::replacePathReferences(selectedFilePath, destination);
    APP_STATE.saveToFile();
  }
  if (destinationPath != nullptr) {
    *destinationPath = destination;
  }
  return true;
}

bool MyLibraryActivity::deleteFile(const std::string& path) {
  if (path.empty()) return false;
  esp_task_wdt_reset();
  if (isBookFile(path)) {
    RECENT_BOOKS.removeBook(path);
    if (StringUtils::checkFileExtension(path, ".epub")) {
      Epub(path, Paths::kDataDir).clearCache();
    } else if (StringUtils::checkFileExtension(path, ".xtc") ||
               StringUtils::checkFileExtension(path, ".xtch")) {
      Xtc(path, Paths::kDataDir).clearCache();
    } else if (StringUtils::checkFileExtension(path, ".txt") ||
               StringUtils::checkFileExtension(path, ".md")) {
      Txt txt(path, Paths::kDataDir);
      Storage.removeDir(txt.getCachePath().c_str());
    }
    esp_task_wdt_reset();
  }

  const bool deleted = Storage.remove(path.c_str());
  LOG_DBG("LIB", "Delete '%s': %s", path.c_str(), deleted ? "ok" : "failed");
  if (deleted) {
    if (isBmpFile(path)) {
      FavoriteBmp::removePathReferences(path);
      APP_STATE.saveToFile();
    }
    if (selectedFilePath == path) selectedFilePath.clear();
  }
  return deleted;
}

bool MyLibraryActivity::deleteSelectedFile() {
  return deleteFile(selectedFilePath);
}

void MyLibraryActivity::requestCleanRefresh() {
  nextRefreshMode = HalDisplay::HALF_REFRESH;
}

void MyLibraryActivity::displayFrame() {
  renderer.displayBuffer(nextRefreshMode);
  nextRefreshMode = HalDisplay::FAST_REFRESH;
}

void MyLibraryActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  loadFiles();
  selectorIndex = 0;
  pendingSearchQuery.clear();
  pendingSearchSubmit = false;
  pendingSearchCancel = false;

  renderer.requestHalfRefresh();
  requestUpdate();
}

void MyLibraryActivity::onExit() {
  ActivityWithSubactivity::onExit();
  files.clear();
  filteredFileIndexes.clear();
  activeSearchQuery.clear();
  pendingSearchQuery.clear();
  pendingSearchSubmit = false;
  pendingSearchCancel = false;
}

void MyLibraryActivity::loop() {
  if (subActivity) { loopSubActivity(); return; }
  if (messagePopupOpen) { loopMessagePopup(); return; }
  if (mode == Mode::BMP_VIEW) { loopBmpView(); return; }
  if (mode == Mode::FILE_ACTIONS) { loopFileActions(); return; }
  if (mode == Mode::FILE_MOVE_BROWSER) { loopFileMoveBrowser(); return; }
  loopBrowse();
}

void MyLibraryActivity::loopSubActivity() {
  subActivity->loop();
  if (pendingSearchSubmit || pendingSearchCancel) {
    const bool shouldApplySearch = pendingSearchSubmit;
    const std::string submittedQuery = pendingSearchQuery;
    pendingSearchSubmit = false;
    pendingSearchCancel = false;
    pendingSearchQuery.clear();
    exitActivity();
    if (shouldApplySearch) {
      setSearchQuery(submittedQuery);
    }
    requestUpdate();
  }
}

void MyLibraryActivity::loopMessagePopup() {
  const bool anyPress = mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
                        mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                        mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
                        mappedInput.wasPressed(MappedInputManager::Button::PageForward) ||
                        mappedInput.wasPressed(MappedInputManager::Button::Left) ||
                        mappedInput.wasPressed(MappedInputManager::Button::Right) ||
                        mappedInput.wasPressed(MappedInputManager::Button::Power);
  if (anyPress) {
    messagePopupOpen = false;
    messagePopupText.clear();
    requestUpdate();
  }
}

void MyLibraryActivity::loopBmpView() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    mode = Mode::BROWSE;
    requestCleanRefresh();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !selectedFilePath.empty()) {
    enterFileActions(selectedFilePath);
  }
}

void MyLibraryActivity::loopFileActions() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    mode = Mode::BROWSE;
    requestCleanRefresh();
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    fileActionIndex = ButtonNavigator::nextIndex(fileActionIndex, getFileActionCount());
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    fileActionIndex = ButtonNavigator::previousIndex(fileActionIndex, getFileActionCount());
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (isBmpFile(selectedFilePath)) {
      switch (fileActionIndex) {
        case 0:
          mode = Mode::BMP_VIEW;
          break;
        case 1:
          enterFileMoveBrowser();
          break;
        case 2: {
          if (!FavoriteBmp::canPlacePathInSleep(selectedFilePath)) {
            showMessagePopup(FavoriteBmp::limitReachedPopupMessage());
            return;
          }
          StatusPopup::showBlocking(renderer, "Moving to sleep");
          std::string destinationPath;
          if (moveSelectedFileTo("/sleep", &destinationPath)) {
            SleepActivity::trimSleepFolderToLimit();
            StatusPopup::showConfirmation(renderer, "Moved");
            mode = Mode::BROWSE;
            if (const auto rawIndex = rawFileIndexForPath(selectedFilePath);
                rawIndex.has_value()) {
              files.erase(files.begin() + static_cast<long>(*rawIndex));
              rebuildFilteredFileIndexes();
              clampSelectorIndex();
            }
          }
          requestCleanRefresh();
          break;
        }
        case 3: {
          const bool makeFavorite = !FavoriteBmp::isFavoritePath(selectedFilePath);
          StatusPopup::showBlocking(renderer, makeFavorite ? "Favoriting" : "Unfavoriting");
          std::string updatedPath;
          const auto result = FavoriteBmp::setFavorite(selectedFilePath, makeFavorite, &updatedPath);
          if (result == FavoriteBmp::SetFavoriteResult::Success) {
            const std::string newName = getBasename(updatedPath);
            if (const auto rawIndex = rawFileIndexForPath(selectedFilePath);
                rawIndex.has_value()) {
              files[*rawIndex] = newName;
              sortFileList(files);
              rebuildFilteredFileIndexes();
              selectorIndex = listIndexForRawFileIndex(findEntry(newName));
            }
            selectedFilePath = updatedPath;
            StatusPopup::showConfirmation(renderer, makeFavorite ? "Favorited" : "Unfavorited");
          } else if (result == FavoriteBmp::SetFavoriteResult::LimitReached) {
            showMessagePopup(FavoriteBmp::limitReachedPopupMessage());
            return;
          } else if (result == FavoriteBmp::SetFavoriteResult::RenameConflict) {
            showMessagePopup("Favorite name already exists");
            return;
          } else if (result == FavoriteBmp::SetFavoriteResult::RenameFailed) {
            showMessagePopup("Failed to rename image");
            return;
          } else {
            showMessagePopup("Favorite failed");
            return;
          }
          requestCleanRefresh();
          break;
        }
        case 4:
          exitActivity();
          enterNewActivity(new QRShareActivity(renderer, mappedInput,
              [this] { exitActivity(); requestCleanRefresh(); }, selectedFilePath));
          return;
        case 5: {
          const std::string pathToDelete = selectedFilePath;
          const std::string fileName = getBasename(pathToDelete);
          enterNewActivity(new ConfirmDialogActivity(
              renderer, mappedInput, "Delete file?\n" + fileName,
              [this, pathToDelete]() {
                StatusPopup::showBlocking(renderer, "Deleting file");
                if (deleteFile(pathToDelete)) {
                  StatusPopup::showConfirmation(renderer, "Deleted");
                  if (const auto rawIndex = rawFileIndexForPath(pathToDelete);
                      rawIndex.has_value()) {
                    files.erase(files.begin() + static_cast<long>(*rawIndex));
                    rebuildFilteredFileIndexes();
                    clampSelectorIndex();
                  }
                } else {
                  LOG_ERR("LIB", "Failed to delete: %s", pathToDelete.c_str());
                  StatusPopup::showConfirmation(renderer, "Delete failed");
                }
                mode = Mode::BROWSE;
                requestCleanRefresh();
                // exitActivity() destroys the subActivity (and this lambda's
                // closure), but `this` points to the parent MyLibraryActivity
                // which is still alive — so requestUpdate() after is safe.
                // It MUST come after exitActivity() because the parent's
                // render task ignores updates while a subActivity exists.
                exitActivity();
                requestUpdate();
              },
              [this]() {
                exitActivity();
                requestUpdate();
              }));
          return;
        }
        default:
          mode = Mode::BROWSE;
          break;
      }
    } else {
      switch (fileActionIndex) {
        case 0:
          onSelectBook(selectedFilePath);
          return;
        case 1:
          enterFileMoveBrowser();
          break;
        case 2:
          exitActivity();
          enterNewActivity(new QRShareActivity(renderer, mappedInput,
              [this] { requestCleanRefresh(); exitActivity(); }, selectedFilePath));
          return;
        case 3: {
          const std::string pathToDelete = selectedFilePath;
          const std::string fileName = getBasename(pathToDelete);
          enterNewActivity(new ConfirmDialogActivity(
              renderer, mappedInput, "Delete file?\n" + fileName,
              [this, pathToDelete]() {
                StatusPopup::showBlocking(renderer, "Deleting file");
                if (deleteFile(pathToDelete)) {
                  StatusPopup::showConfirmation(renderer, "Deleted");
                  progressPrefixCache.erase(pathToDelete);
                  if (const auto rawIndex = rawFileIndexForPath(pathToDelete);
                      rawIndex.has_value()) {
                    files.erase(files.begin() + static_cast<long>(*rawIndex));
                    rebuildFilteredFileIndexes();
                    clampSelectorIndex();
                  }
                } else {
                  LOG_ERR("LIB", "Failed to delete: %s", pathToDelete.c_str());
                  StatusPopup::showConfirmation(renderer, "Delete failed");
                }
                mode = Mode::BROWSE;
                requestCleanRefresh();
                exitActivity();
                requestUpdate();
              },
              [this]() {
                exitActivity();
                requestUpdate();
              }));
          return;
        }
        default:
          mode = Mode::BROWSE;
          break;
      }
    }
    requestUpdateAndWait();
  }
}

void MyLibraryActivity::loopFileMoveBrowser() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    mode = Mode::FILE_ACTIONS;
    requestCleanRefresh();
    requestUpdate();
    return;
  }

  const int targetCount = static_cast<int>(moveBrowseEntries.size());
  if (targetCount <= 0) {
    mode = Mode::FILE_ACTIONS;
    requestCleanRefresh();
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this, targetCount] {
    fileMoveIndex = ButtonNavigator::nextIndex(fileMoveIndex, targetCount);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, targetCount] {
    fileMoveIndex = ButtonNavigator::previousIndex(fileMoveIndex, targetCount);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto& entry = moveBrowseEntries[fileMoveIndex];
    if (entry.path == "/sleep" &&
        !FavoriteBmp::canPlacePathInSleep(selectedFilePath)) {
      showMessagePopup(FavoriteBmp::limitReachedPopupMessage());
      return;
    }
    StatusPopup::showBlocking(renderer, "Moving file");
    std::string destinationPath;
    if (moveSelectedFileTo(entry.path, &destinationPath)) {
      if (entry.path == "/sleep") {
        SleepActivity::trimSleepFolderToLimit();
      }
      StatusPopup::showConfirmation(renderer, "Moved");
      mode = Mode::BROWSE;
      progressPrefixCache.erase(selectedFilePath);
      if (const auto rawIndex = rawFileIndexForPath(selectedFilePath);
          rawIndex.has_value()) {
        files.erase(files.begin() + static_cast<long>(*rawIndex));
        rebuildFilteredFileIndexes();
        clampSelectorIndex();
      }
    } else {
      // Keep the destination list open after a failed move.
      loadMoveBrowseEntries();
    }
    requestCleanRefresh();
    requestUpdateAndWait();
  }
}

void MyLibraryActivity::loopBrowse() {
  // Long press BACK (1s+) goes to root folder — fires while held
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS &&
      basepath != "/") {
    mappedInput.suppressUntilAllReleased();
    basepath = "/";
    StatusPopup::showBlocking(renderer, "Opening...");
    loadFiles();
    clearSearch();
    selectorIndex = 0;
    requestUpdate();
    return;
  }

  // Long press CONFIRM (1s+) on a file — opens file actions while held
  {
    const auto rawIndex = rawFileIndexForListIndex(selectorIndex);
    if (rawIndex.has_value() &&
        mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        mappedInput.getHeldTime() >= GO_HOME_MS &&
        isManagedFile(files[*rawIndex])) {
      const std::string selectedPath = makeAbsolutePath(files[*rawIndex]);
      mappedInput.suppressUntilAllReleased();
      enterFileActions(selectedPath);
      return;
    }
  }

  const int pageItems = BaseTheme::getNumberOfItemsPerPage(renderer, true, false, true, false);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (isSearchActionRow(selectorIndex)) {
      openSearchActivity();
      return;
    }

    if (isClearSearchRow(selectorIndex)) {
      clearSearch();
      selectorIndex = 0;
      requestUpdate();
      return;
    }

    if (isLoadMoreRow(selectorIndex)) {
      loadMoreFiles();
      requestUpdate();
      return;
    }

    const auto rawIndex = rawFileIndexForListIndex(selectorIndex);
    if (!rawIndex.has_value()) {
      return;
    }

    const std::string selectedEntry = files[*rawIndex];
    const std::string selectedPath = makeAbsolutePath(selectedEntry);

    if (selectedEntry.back() == '/') {
      clearSearch();
      basepath = selectedPath.substr(0, selectedPath.length() - 1);
      StatusPopup::showBlocking(renderer, "Opening...");
      loadFiles();
      selectorIndex = 0;
      requestCleanRefresh();
      requestUpdate();
    } else if (isManagedFile(selectedEntry)) {
      if (isBmpFile(selectedEntry)) {
        enterBmpView(selectedPath);
      } else {
        onSelectBook(selectedPath);
        return;
      }
    } else {
      onSelectBook(selectedPath);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (basepath != "/") {
      const std::string oldPath = basepath;

      const auto lastSlash = basepath.find_last_of('/');
      if (lastSlash != std::string::npos) basepath.replace(lastSlash, std::string::npos, "");
      if (basepath.empty()) basepath = "/";
      StatusPopup::showBlocking(renderer, "Opening...");
      loadFiles();
      clearSearch();

      const auto pos = oldPath.find_last_of('/');
      const std::string dirName = oldPath.substr(pos + 1) + "/";
      selectorIndex = listIndexForRawFileIndex(findEntry(dirName));

      requestCleanRefresh();
      requestUpdate();
    } else {
      onGoHome();
    }
  }

  const int listSize = static_cast<int>(totalListCount());

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

void MyLibraryActivity::render(Activity::RenderLock&&) {
  // Reset stacking so render-loop popups start at the top.
  TransitionFeedback::resetStacking();

  if (mode == Mode::BMP_VIEW) {
    renderBmpView();
    return;
  }

  if (mode == Mode::FILE_ACTIONS) {
    renderFileActions();
    return;
  }

  if (mode == Mode::FILE_MOVE_BROWSER) {
    renderFileMoveBrowser();
    return;
  }

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = BaseMetrics::values;

  std::string folderName = (basepath == "/") ? tr(STR_SD_CARD) : basepath.substr(basepath.rfind('/') + 1);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int pathY = contentTop;
  const int pathWidth = pageWidth - metrics.contentSidePadding * 2;
  const std::string pathLabel =
      renderer.truncatedText(SMALL_FONT_ID, basepath.c_str(), pathWidth);
  renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, pathY,
                    pathLabel.c_str());

  const int listTop = pathY + renderer.getLineHeight(SMALL_FONT_ID) + 2;
  const int listHeight =
      pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int rowGap = 1;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int rowPadY = 2;
  const int listX = 0;
  const int listW = pageWidth;
  const int textX = listX + metrics.contentSidePadding;
  const int textW = listW - metrics.contentSidePadding * 2 - 3;

  clampSelectorIndex();
  const int selected = static_cast<int>(selectorIndex);
  const int totalRows = static_cast<int>(totalListCount());

  int startIndex = selected;
  int usedHeight = 0;
  for (int i = selected; i >= 0; --i) {
    const auto bwLines = wrapTextToWidth(renderer, UI_10_FONT_ID,
                                         getRowTextForListIndex(i), textW);
    const int bwHeight =
        static_cast<int>(bwLines.size()) * lineHeight + rowPadY * 2;
    const int blockHeight = bwHeight + (usedHeight > 0 ? rowGap : 0);
    if (usedHeight + blockHeight > listHeight) {
      break;
    }
    usedHeight += blockHeight;
    startIndex = i;
  }

  int y = listTop;
  int lastVisibleIndex = startIndex - 1;
  for (int i = startIndex; i < totalRows; ++i) {
    const auto wrappedLines = wrapTextToWidth(
        renderer, UI_10_FONT_ID, getRowTextForListIndex(i), textW);
    const int rowHeight =
        static_cast<int>(wrappedLines.size()) * lineHeight + rowPadY * 2;

    if (y + rowHeight > listTop + listHeight) {
      break;
    }

    const bool isSelected = i == selected;
    if (isSelected) {
      renderer.fillRect(listX, y, listW, rowHeight, true);
    }
    int lineY = y + rowPadY;
    for (const auto& line : wrappedLines) {
      renderer.drawText(UI_10_FONT_ID, textX, lineY, line.c_str(), !isSelected);
      lineY += lineHeight;
    }
    lastVisibleIndex = i;
    y += rowHeight + rowGap;
  }

  const bool hasMoreAbove = startIndex > 0;
  const bool hasMoreBelow = lastVisibleIndex + 1 < totalRows;
  const int markerFont = UI_12_FONT_ID;
  const int markerLineHeight = renderer.getLineHeight(markerFont);
  const int markerRightMargin = 6;

  if (hasMoreAbove) {
    const char* topMarker = "<";
    const int markerW = renderer.getTextWidth(markerFont, topMarker);
    const int markerX = pageWidth - markerRightMargin - markerW;
    renderer.drawText(markerFont, markerX, listTop, topMarker);
  }

  if (hasMoreBelow) {
    const char* bottomMarker = ">";
    const int markerW = renderer.getTextWidth(markerFont, bottomMarker);
    const int markerX = pageWidth - markerRightMargin - markerW;
    const int markerY = std::max(
        listTop, std::min(listTop + listHeight - markerLineHeight,
                          y - markerLineHeight));
    renderer.drawText(markerFont, markerX, markerY, bottomMarker);
  }

  // Help text
  const auto selectedRawIndex = rawFileIndexForListIndex(selectorIndex);
  const bool hasSelectedFile =
      selectedRawIndex.has_value() && isManagedFile(files[*selectedRawIndex]);
  const bool hasSelectedBmp =
      selectedRawIndex.has_value() && isBmpFile(files[*selectedRawIndex]);
  const char* confirmLabel = tr(STR_OPEN);
  if (isSearchActionRow(selectorIndex)) {
    confirmLabel = "Search";
  } else if (isClearSearchRow(selectorIndex)) {
    confirmLabel = "Clear";
  } else if (hasSelectedFile) {
    confirmLabel = hasSelectedBmp ? "View\n/hold" : "Open\n/hold";
  }
  const char* backLabel = basepath == "/" ? tr(STR_HOME) : "Back\n/hold";
  const auto labels = mappedInput.mapLabels(backLabel,
                                            confirmLabel, tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (messagePopupOpen) {
    GUI.drawPopup(renderer, messagePopupText.c_str());
  }

  displayFrame();
}

void MyLibraryActivity::renderBmpView() {
  renderer.clearScreen();

  FsFile file;
  if (!Storage.openFileForRead("LIB", selectedFilePath, file)) {
    renderer.drawCenteredText(UI_12_FONT_ID, 80, "Failed to open BMP");
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    displayFrame();
    return;
  }

  Bitmap bitmap(file, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    renderer.drawCenteredText(UI_12_FONT_ID, 80, "Invalid BMP");
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    displayFrame();
    return;
  }

  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  int x = 0;
  int y = 0;

  if (bitmap.getWidth() > screenW || bitmap.getHeight() > screenH) {
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(screenW) / static_cast<float>(screenH);
    if (ratio > screenRatio) {
      y = std::round((static_cast<float>(screenH) - static_cast<float>(screenW) / ratio) / 2.0f);
    } else {
      x = std::round((static_cast<float>(screenW) - static_cast<float>(screenH) * ratio) / 2.0f);
    }
  } else {
    x = (screenW - bitmap.getWidth()) / 2;
    y = (screenH - bitmap.getHeight()) / 2;
  }

  const bool hasGreyscale = bitmap.hasGreyscale();

  renderer.drawBitmap(bitmap, x, y, screenW, screenH, 0.0f, 0.0f);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Actions", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (messagePopupOpen) {
    GUI.drawPopup(renderer, messagePopupText.c_str());
  }

  // Full refresh for all images — clears previous screen artifacts from
  // white/light areas that HALF_REFRESH and FAST_REFRESH leave behind.
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  nextRefreshMode = HalDisplay::FAST_REFRESH;

  if (hasGreyscale) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, screenW, screenH, 0.0f, 0.0f);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, screenW, screenH, 0.0f, 0.0f);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

void MyLibraryActivity::renderFileActions() {
  renderer.clearScreen();
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int popupX = 24;
  const int popupY = 32;
  const int popupW = screenW - popupX * 2;
  const int popupH = screenH - popupY * 2;

  renderer.drawRect(popupX, popupY, popupW, popupH, true);
  renderer.drawCenteredText(UI_12_FONT_ID, popupY + 10, "File Actions", true, EpdFontFamily::REGULAR);

  const int rowStartY = popupY + 44;
  const int rowH = 26;
  const int actionCount = getFileActionCount();
  for (int i = 0; i < actionCount; i++) {
    const int rowY = rowStartY + i * rowH;
    const bool selected = (i == fileActionIndex);
    if (selected) {
      renderer.fillRect(popupX + 8, rowY - 2, popupW - 16, rowH, true);
    }
    const std::string label = getFileActionLabel(i);
    renderer.drawText(UI_10_FONT_ID, popupX + 14, rowY, label.c_str(), !selected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (messagePopupOpen) {
    GUI.drawPopup(renderer, messagePopupText.c_str());
  }
  displayFrame();
}

void MyLibraryActivity::renderFileMoveBrowser() {
  renderer.clearScreen();
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const auto metrics = BaseMetrics::values;
  const int popupX = 12;
  const int popupY = 12;
  const int popupW = screenW - popupX * 2;
  const int popupBottom = screenH - metrics.buttonHintsHeight - 6;
  const int popupH = std::max(80, popupBottom - popupY);

  renderer.drawRect(popupX, popupY, popupW, popupH, true);
  std::string title = "Move To Folder";
  title = renderer.truncatedText(UI_12_FONT_ID, title.c_str(), popupW - 24);
  renderer.drawCenteredText(UI_12_FONT_ID, popupY + 8, title.c_str(), true, EpdFontFamily::REGULAR);

  const int rowStartY = popupY + 30;
  const int rowH = renderer.getLineHeight(UI_10_FONT_ID) + 8;
  const int rowBottomY = popupY + popupH - 8;
  const int maxRows = std::max(1, (rowBottomY - rowStartY) / rowH);
  int startIndex = 0;
  if (fileMoveIndex >= maxRows) {
    startIndex = fileMoveIndex - maxRows + 1;
  }

  for (int row = 0; row < maxRows; row++) {
    const int idx = startIndex + row;
    if (idx >= static_cast<int>(moveBrowseEntries.size())) break;
    const int rowY = rowStartY + row * rowH;
    const bool selected = (idx == fileMoveIndex);
    if (selected) {
      renderer.fillRect(popupX + 8, rowY - 2, popupW - 16, rowH, true);
    }
    std::string label = renderer.truncatedText(UI_10_FONT_ID, moveBrowseEntries[idx].name.c_str(), popupW - 26);
    renderer.drawText(UI_10_FONT_ID, popupX + 13, rowY, label.c_str(), !selected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (messagePopupOpen) {
    GUI.drawPopup(renderer, messagePopupText.c_str());
  }
  displayFrame();
}

size_t MyLibraryActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}
