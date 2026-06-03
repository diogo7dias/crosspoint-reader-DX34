#include "MyLibraryActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Txt.h>
#include <Xtc.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <cmath>
#include <random>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "LibraryListingFilter.h"
#include "LibrarySearchActivity.h"
#include "LibrarySearchSupport.h"
#include "MappedInputManager.h"
#include "Paths.h"
#include "activities/boot_sleep/SleepActivity.h"
#include "activities/network/QRShareActivity.h"
#include "activities/reader/ReaderLayoutSafety.h"
#include "activities/util/ConfirmDialogActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "persist/Trash.h"
#include "util/BookProgress.h"
#include "util/FavoriteImage.h"
#include "util/PxcRenderer.h"
#include "util/StatusPopup.h"
#include "util/StringUtils.h"
#include "util/TransitionFeedback.h"

void sortFileList(std::vector<std::string>& strs);

extern HalGPIO gpio;

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr size_t kDefaultFileLimit = 200;
constexpr size_t kFilesPerBatch = 200;
constexpr size_t kFileLoadProgressThreshold = 50;
constexpr size_t kFileLoadProgressInterval = 100;

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
  return equalsIgnoreCaseAscii(folderName, "book") || equalsIgnoreCaseAscii(folderName, "books");
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

void collectMoveDestinationPaths(const std::string& basePath, std::vector<std::string>& outPaths, int depth = 0) {
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

// Thin adapter over ReaderLayoutSafety::wrapText. The list renderer measures row height from
// `.size()`, so an empty-text row must still return a single (empty) line — otherwise the row
// collapses to just its vertical padding. wrapText returns `{}` for empty input, so we normalize.
std::vector<std::string> wrapTextToWidth(const GfxRenderer& renderer, const int fontId, const std::string& text,
                                         const int maxWidth) {
  auto lines = ReaderLayoutSafety::wrapText(renderer, fontId, text, maxWidth);
  if (lines.empty()) {
    lines.emplace_back();
  }
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
        const bool relevant = !skip && (next.isDirectory() || isManagedFile(std::string(name)));
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

  // Remove EPUB-render cache files before any ordering/capping logic.
  filterEpubCachePxc(files);

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
      [this]() { pendingSearchCancel = true; }));
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

  filteredFileIndexes = LibrarySearchSupport::rankMatches(files, activeSearchQuery);
}

bool MyLibraryActivity::hasActiveSearch() const { return !activeSearchQuery.empty(); }

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

bool MyLibraryActivity::isSearchActionRow(const size_t listIndex) const { return folderHasBooks && listIndex == 0; }

bool MyLibraryActivity::isClearSearchRow(const size_t listIndex) const { return hasActiveSearch() && listIndex == 1; }

bool MyLibraryActivity::isLoadMoreRow(const size_t listIndex) const {
  return hasMoreFiles && !hasActiveSearch() && listIndex == entryListOffset() + visibleEntryCount();
}

std::optional<size_t> MyLibraryActivity::rawFileIndexForListIndex(const size_t listIndex) const {
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

std::optional<size_t> MyLibraryActivity::rawFileIndexForPath(const std::string& path) const {
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

bool MyLibraryActivity::isPxcFile(const std::string& filename) {
  return StringUtils::checkFileExtension(filename, ".pxc");
}

bool MyLibraryActivity::isImageFile(const std::string& filename) { return isBmpFile(filename) || isPxcFile(filename); }

bool MyLibraryActivity::isManagedFile(const std::string& filename) {
  return isBookFile(filename) || isImageFile(filename);
}

void MyLibraryActivity::filterEpubCachePxc(std::vector<std::string>& files) {
  LibraryListingFilter::filterEpubCachePxc(files);
}

std::string MyLibraryActivity::getDisplayNameForRawFile(const size_t rawIndex) {
  if (rawIndex >= files.size()) {
    return "";
  }

  const std::string& name = files[rawIndex];
  if (!name.empty() && name.back() == '/') {
    return "[" + name.substr(0, name.size() - 1) + "]";
  }

  if (isImageFile(name)) {
    return FavoriteImage::displayNameForPath(makeAbsolutePath(name));
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
      // Most library entries are also in RECENT_BOOKS (any book ever opened
      // is tracked), and the reader keeps that percent fresh on exit and
      // page turn. Hitting it first avoids the per-row EPUB spine+TOC parse
      // + progress.bin read that BookProgress::getPercent does, which was
      // the dominant cost on library scroll.
      const int recentPercent = RECENT_BOOKS.getCachedPercent(fullPath);
      std::optional<int> percent;
      if (recentPercent >= 0) {
        percent = recentPercent;
      } else {
        // Untracked book (never opened): fall back to disk parse.
        percent = BookProgress::getPercent(fullPath);
      }
      cached = progressPrefixCache.emplace(fullPath, formatLibraryProgressPrefix(percent)).first;
    }
    rowText = cached->second + "  " + rowText;
  }

  return rowText;
}

void MyLibraryActivity::enterImageView(const std::string& imagePath) {
  // Single "Opening image" toast — PXC renders fast enough that a stacked
  // load toast would just race the image onto the screen. Full refresh on
  // enter scrubs the list ghost.
  if (!TransitionFeedback::isActive()) {
    TransitionFeedback::show(renderer, tr(STR_OPENING_IMAGE));
  }
  selectedFilePath = imagePath;
  mode = Mode::IMAGE_VIEW;
  imageViewFullyLoaded = false;
  renderer.requestFullRefresh();
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

void MyLibraryActivity::openKeyboardForRenameImage() {
  if (!isImageFile(selectedFilePath)) return;

  pendingRenameBase.clear();
  pendingRenameSubmit = false;
  pendingRenameCancel = false;
  enterNewActivity(new KeyboardEntryActivity(
      renderer, mappedInput, tr(STR_RENAME_IMAGE_TITLE), "", 10, 200, false,
      [this](const std::string& newBase) {
        pendingRenameBase = newBase;
        pendingRenameSubmit = true;
      },
      [this]() { pendingRenameCancel = true; }));
}

void MyLibraryActivity::renameSelectedImage(const std::string& newBase) {
  std::string base = newBase;
  // Trim whitespace.
  while (!base.empty() && (base.front() == ' ' || base.front() == '\t')) base.erase(base.begin());
  while (!base.empty() && (base.back() == ' ' || base.back() == '\t')) base.pop_back();

  // Capture the original extension up-front so the rebuilt path keeps it.
  std::string originalExt = ".bmp";
  if (selectedFilePath.size() >= 4) {
    std::string tail = selectedFilePath.substr(selectedFilePath.size() - 4);
    std::transform(tail.begin(), tail.end(), tail.begin(), ::tolower);
    if (tail == ".pxc") originalExt = ".pxc";
  }

  // Strip a user-typed trailing .bmp/.pxc (any case) and _F — we control both.
  if (base.size() >= 4) {
    std::string tail = base.substr(base.size() - 4);
    std::transform(tail.begin(), tail.end(), tail.begin(), ::tolower);
    if (tail == ".bmp" || tail == ".pxc") base = base.substr(0, base.size() - 4);
  }
  if (base.size() >= 2 && base.substr(base.size() - 2) == "_F") {
    base = base.substr(0, base.size() - 2);
  }

  if (base.empty()) {
    showMessagePopup(tr(STR_INVALID_NAME));
    return;
  }
  for (char c : base) {
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
        static_cast<unsigned char>(c) < 0x20) {
      showMessagePopup(tr(STR_INVALID_NAME));
      return;
    }
  }

  const bool wasFav = FavoriteImage::isFavoritePath(selectedFilePath);
  const std::string parent = basepath.empty() ? "/" : basepath;
  const std::string parentSlash = (parent.back() == '/') ? parent : parent + "/";
  const std::string suffix = std::string(wasFav ? "_F" : "") + originalExt;

  auto buildPath = [&](const std::string& b) { return parentSlash + b + suffix; };

  std::string targetPath = buildPath(base);
  if (targetPath == selectedFilePath) {
    mode = actionsOpenedFromViewer ? Mode::IMAGE_VIEW : Mode::FILE_ACTIONS;
    requestUpdate();
    return;
  }

  // Auto-suffix collision: _1, _2, ...
  if (Storage.exists(targetPath.c_str())) {
    bool found = false;
    for (int n = 1; n <= 999; ++n) {
      const std::string candidate = buildPath(base + "_" + std::to_string(n));
      if (!Storage.exists(candidate.c_str())) {
        targetPath = candidate;
        found = true;
        break;
      }
    }
    if (!found) {
      showMessagePopup(tr(STR_TOO_MANY_SIMILAR_NAMES));
      return;
    }
  }

  StatusPopup::showBlocking(renderer, tr(STR_RENAMING));
  if (!Storage.rename(selectedFilePath.c_str(), targetPath.c_str())) {
    showMessagePopup(tr(STR_RENAME_FAILED));
    return;
  }

  FavoriteImage::replacePathReferences(selectedFilePath, targetPath);
  APP_STATE.saveToFile();

  const std::string newName = getBasename(targetPath);
  if (const auto rawIndex = rawFileIndexForPath(selectedFilePath); rawIndex.has_value()) {
    files[*rawIndex] = newName;
    sortFileList(files);
    rebuildFilteredFileIndexes();
    selectorIndex = listIndexForRawFileIndex(findEntry(newName));
  }
  selectedFilePath = targetPath;
  StatusPopup::showConfirmation(renderer, tr(STR_RENAMED));

  // Close the actions menu and return to the file list after rename.
  actionsOpenedFromViewer = false;
  mode = Mode::BROWSE;
  requestCleanRefresh();
  requestUpdateAndWait();
}

int MyLibraryActivity::getFileActionCount() const {
  if (!isImageFile(selectedFilePath)) return 5;
  return actionsOpenedFromViewer ? 7 : 8;
}

std::string MyLibraryActivity::getFileActionLabel(const int index) const {
  if (isImageFile(selectedFilePath)) {
    int i = index;
    if (!actionsOpenedFromViewer) {
      if (i == 0) return tr(STR_OPEN_IMAGE);
      i -= 1;
    }
    switch (i) {
      case 0:
        return tr(STR_MOVE_TO_SLEEP_ACTION);
      case 1:
        return FavoriteImage::isFavoritePath(selectedFilePath) ? tr(STR_UNFAVORITE) : tr(STR_FAVORITE);
      case 2:
        return tr(STR_RENAME_ACTION);
      case 3:
        return tr(STR_MOVE_FILE_ACTION);
      case 4:
        return tr(STR_DOWNLOAD_IMAGE_VIA_QR);
      case 5:
        return tr(STR_DELETE_FILE_ACTION);
      default:
        return tr(STR_CANCEL);
    }
  }

  switch (index) {
    case 0:
      return tr(STR_OPEN_BOOK);
    case 1:
      return tr(STR_MOVE_FILE_ACTION);
    case 2:
      return tr(STR_DOWNLOAD_BOOK_VIA_QR);
    case 3:
      return tr(STR_DELETE_FILE_ACTION);
    default:
      return tr(STR_CANCEL);
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
  std::transform(destinationPaths.begin(), destinationPaths.end(), std::back_inserter(moveBrowseEntries),
                 [](const std::string& path) { return MoveBrowseEntry{path, path, false, false}; });

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

bool MyLibraryActivity::moveSelectedFileTo(const std::string& targetDir, std::string* destinationPath) const {
  if (selectedFilePath.empty()) return false;

  std::string normalizedTarget = targetDir;
  if (normalizedTarget.empty()) normalizedTarget = "/";
  if (normalizedTarget.back() != '/') normalizedTarget += "/";
  if (normalizedTarget == "/sleep/" && !FavoriteImage::canPlacePathInSleep(selectedFilePath)) {
    return false;
  }

  const std::string filename = getBasename(selectedFilePath);
  const std::string destination = normalizedTarget + filename;

  if (destination == selectedFilePath || Storage.exists(destination.c_str())) {
    return false;
  }

  // Build QUOTES sidecar paths up front so we can carry it along with a book.
  std::string quotesOld;
  std::string quotesNew;
  if (isBookFile(selectedFilePath)) {
    const auto dotOld = selectedFilePath.rfind('.');
    const std::string baseOld = (dotOld != std::string::npos) ? selectedFilePath.substr(0, dotOld) : selectedFilePath;
    quotesOld = baseOld + "_QUOTES.txt";
    const auto dotNew = destination.rfind('.');
    const std::string baseNew = (dotNew != std::string::npos) ? destination.substr(0, dotNew) : destination;
    quotesNew = baseNew + "_QUOTES.txt";
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
    // Carry _QUOTES.txt sidecar along with the book so quotes stay attached.
    if (!quotesOld.empty() && Storage.exists(quotesOld.c_str())) {
      if (Storage.rename(quotesOld.c_str(), quotesNew.c_str())) {
        LOG_DBG("LIB", "Moved QUOTES sidecar: %s -> %s", quotesOld.c_str(), quotesNew.c_str());
      } else {
        LOG_ERR("LIB", "Failed to move QUOTES sidecar: %s", quotesOld.c_str());
      }
    }
    RECENT_BOOKS.removeBook(selectedFilePath);
  } else if (isImageFile(selectedFilePath)) {
    FavoriteImage::replacePathReferences(selectedFilePath, destination);
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
    esp_task_wdt_reset();
  }

  // Move into /.crosspoint/trash/ instead of hard-deleting so the user has
  // a window to recover. trash::moveToTrash handles cache dir + QUOTES.txt
  // for books.
  const bool moved = trash::moveToTrash(path);
  if (!moved) {
    LOG_ERR("LIB", "trash::moveToTrash failed for %s, not hard-deleting", path.c_str());
    return false;
  }

  if (isImageFile(path)) {
    FavoriteImage::removePathReferences(path);
    APP_STATE.saveToFile();
  }
  if (selectedFilePath == path) selectedFilePath.clear();
  return true;
}

bool MyLibraryActivity::deleteSelectedFile() { return deleteFile(selectedFilePath); }

void MyLibraryActivity::requestCleanRefresh() { nextRefreshMode = HalDisplay::HALF_REFRESH; }

void MyLibraryActivity::displayFrame() {
  renderer.displayBuffer(nextRefreshMode);
  nextRefreshMode = HalDisplay::FAST_REFRESH;
}

void MyLibraryActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  // A3 paint-then-load: defer the SD directory scan to the next loop tick so
  // we paint a "Loading library…" placeholder first instead of blocking the
  // first frame for 200–800 ms on large folders. Hot-path callers of
  // loadFiles() (rename / delete / loadMore) keep synchronous behaviour.
  files.clear();
  filteredFileIndexes.clear();
  hasMoreFiles = false;
  folderHasBooks = false;
  pendingLibraryLoad = true;

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
  // A3 paint-then-load: first tick after onEnter — placeholder frame already
  // painted, now run the actual SD scan. Skip input handling entirely until
  // the load completes so the user cannot act on a stale empty list.
  if (pendingLibraryLoad) {
    loadFiles();
    pendingLibraryLoad = false;
    rebuildFilteredFileIndexes();
    requestUpdate();
    return;
  }

  if (subActivity) {
    loopSubActivity();
    return;
  }
  if (messagePopupOpen) {
    loopMessagePopup();
    return;
  }
  if (mode == Mode::IMAGE_VIEW) {
    loopImageView();
    return;
  }
  if (mode == Mode::FILE_ACTIONS) {
    loopFileActions();
    return;
  }
  if (mode == Mode::FILE_MOVE_BROWSER) {
    loopFileMoveBrowser();
    return;
  }
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
  if (pendingRenameSubmit || pendingRenameCancel) {
    const bool shouldApplyRename = pendingRenameSubmit;
    const std::string typed = pendingRenameBase;
    pendingRenameSubmit = false;
    pendingRenameCancel = false;
    pendingRenameBase.clear();
    exitActivity();
    if (shouldApplyRename) {
      renameSelectedImage(typed);
    } else {
      requestUpdate();
    }
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

void MyLibraryActivity::loopImageView() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    mode = Mode::BROWSE;
    requestCleanRefresh();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !selectedFilePath.empty()) {
    actionsOpenedFromViewer = true;
    enterFileActions(selectedFilePath);
    // FAST_REFRESH (~300ms) — user prefers a snappy menu over the cleaner
    // but slower HALF_REFRESH open. Minor ghost under the popup is the
    // tradeoff.
    nextRefreshMode = HalDisplay::FAST_REFRESH;
  }
}

void MyLibraryActivity::loopFileActions() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (actionsOpenedFromViewer) {
      actionsOpenedFromViewer = false;
      mode = Mode::IMAGE_VIEW;
    } else {
      mode = Mode::BROWSE;
    }
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
    if (isImageFile(selectedFilePath)) {
      // When menu opened from library browser, index 0 is "Open Image" and
      // the remaining actions start at 1. When opened from the viewer,
      // "Open Image" is hidden and actions start at 0. Shift to a unified
      // action index so the switch below stays simple.
      if (!actionsOpenedFromViewer && fileActionIndex == 0) {
        mode = Mode::IMAGE_VIEW;
        requestUpdateAndWait();
        return;
      }
      const int actionIndex = fileActionIndex - (actionsOpenedFromViewer ? 0 : 1);
      switch (actionIndex) {
        case 0: {
          if (!FavoriteImage::canPlacePathInSleep(selectedFilePath)) {
            showMessagePopup(FavoriteImage::limitReachedPopupMessage());
            return;
          }
          StatusPopup::showBlocking(renderer, tr(STR_MOVING_TO_SLEEP));
          std::string destinationPath;
          if (moveSelectedFileTo("/sleep", &destinationPath)) {
            SleepActivity::trimSleepFolderToLimit();
            StatusPopup::showConfirmation(renderer, tr(STR_MOVED));
            actionsOpenedFromViewer = false;
            mode = Mode::BROWSE;
            if (const auto rawIndex = rawFileIndexForPath(selectedFilePath); rawIndex.has_value()) {
              files.erase(files.begin() + static_cast<long>(*rawIndex));
              rebuildFilteredFileIndexes();
              clampSelectorIndex();
            }
          }
          requestCleanRefresh();
          break;
        }
        case 1: {
          const bool makeFavorite = !FavoriteImage::isFavoritePath(selectedFilePath);
          StatusPopup::showBlocking(renderer, makeFavorite ? tr(STR_FAVORITING) : tr(STR_UNFAVORITING));
          std::string updatedPath;
          const auto result = FavoriteImage::setFavorite(selectedFilePath, makeFavorite, &updatedPath);
          if (result == FavoriteImage::SetFavoriteResult::Success) {
            const std::string newName = getBasename(updatedPath);
            if (const auto rawIndex = rawFileIndexForPath(selectedFilePath); rawIndex.has_value()) {
              files[*rawIndex] = newName;
              sortFileList(files);
              rebuildFilteredFileIndexes();
              selectorIndex = listIndexForRawFileIndex(findEntry(newName));
            }
            selectedFilePath = updatedPath;
            StatusPopup::showConfirmation(renderer, makeFavorite ? tr(STR_FAVORITED) : tr(STR_UNFAVORITED));
          } else if (result == FavoriteImage::SetFavoriteResult::LimitReached) {
            showMessagePopup(FavoriteImage::limitReachedPopupMessage());
            return;
          } else if (result == FavoriteImage::SetFavoriteResult::RenameConflict) {
            showMessagePopup(tr(STR_FAVORITE_NAME_EXISTS));
            return;
          } else if (result == FavoriteImage::SetFavoriteResult::RenameFailed) {
            showMessagePopup(tr(STR_FAILED_RENAME_IMAGE));
            return;
          } else {
            showMessagePopup(tr(STR_FAVORITE_FAILED));
            return;
          }
          requestCleanRefresh();
          break;
        }
        case 2:
          openKeyboardForRenameImage();
          return;
        case 3:
          actionsOpenedFromViewer = false;
          enterFileMoveBrowser();
          break;
        case 4:
          actionsOpenedFromViewer = false;
          exitActivity();
          enterNewActivity(new QRShareActivity(
              renderer, mappedInput,
              [this] {
                exitActivity();
                requestCleanRefresh();
              },
              selectedFilePath));
          return;
        case 5: {
          const std::string pathToDelete = selectedFilePath;
          const std::string fileName = getBasename(pathToDelete);
          enterNewActivity(new ConfirmDialogActivity(
              renderer, mappedInput, std::string(tr(STR_DELETE_FILE_CONFIRM)) + "\n" + fileName,
              [this, pathToDelete]() {
                // Optimistic UX: snapshot the entry, remove from list
                // before the rename, re-insert on failure. Gives an
                // instant visual response regardless of SD speed.
                TransitionFeedback::show(renderer, tr(STR_DELETING));
                const auto snapIndex = rawFileIndexForPath(pathToDelete);
                std::string snapEntry;
                if (snapIndex.has_value()) {
                  snapEntry = files[*snapIndex];
                  files.erase(files.begin() + static_cast<long>(*snapIndex));
                  rebuildFilteredFileIndexes();
                  clampSelectorIndex();
                }
                TransitionFeedback::show(renderer, tr(STR_MOVING_TO_TRASH));
                if (deleteFile(pathToDelete)) {
                  TransitionFeedback::show(renderer, tr(STR_DELETED));
                } else {
                  LOG_ERR("LIB", "Failed to delete: %s", pathToDelete.c_str());
                  if (snapIndex.has_value()) {
                    files.insert(files.begin() + static_cast<long>(*snapIndex), snapEntry);
                    rebuildFilteredFileIndexes();
                    clampSelectorIndex();
                  }
                  TransitionFeedback::show(renderer, tr(STR_DELETE_FAILED));
                }
                TransitionFeedback::ensureMinDisplayElapsed();
                actionsOpenedFromViewer = false;
                mode = Mode::BROWSE;
                // Block 2: full refresh on delete completion — no ghost
                renderer.requestFullRefresh();
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
          // Cancel — mirror the Back button: return to viewer if that's
          // where the menu came from, otherwise to the file list.
          if (actionsOpenedFromViewer) {
            actionsOpenedFromViewer = false;
            mode = Mode::IMAGE_VIEW;
          } else {
            mode = Mode::BROWSE;
          }
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
          enterNewActivity(new QRShareActivity(
              renderer, mappedInput,
              [this] {
                requestCleanRefresh();
                exitActivity();
              },
              selectedFilePath));
          return;
        case 3: {
          const std::string pathToDelete = selectedFilePath;
          const std::string fileName = getBasename(pathToDelete);
          enterNewActivity(new ConfirmDialogActivity(
              renderer, mappedInput, std::string(tr(STR_DELETE_FILE_CONFIRM)) + "\n" + fileName,
              [this, pathToDelete]() {
                // Optimistic UX: snapshot + remove before the rename so the
                // list reflects the action instantly; re-insert on failure.
                TransitionFeedback::show(renderer, tr(STR_DELETING));
                const auto snapIndex = rawFileIndexForPath(pathToDelete);
                std::string snapEntry;
                if (snapIndex.has_value()) {
                  snapEntry = files[*snapIndex];
                  files.erase(files.begin() + static_cast<long>(*snapIndex));
                  rebuildFilteredFileIndexes();
                  clampSelectorIndex();
                }
                progressPrefixCache.erase(pathToDelete);
                TransitionFeedback::show(renderer, tr(STR_MOVING_TO_TRASH));
                if (deleteFile(pathToDelete)) {
                  TransitionFeedback::show(renderer, tr(STR_DELETED));
                } else {
                  LOG_ERR("LIB", "Failed to delete: %s", pathToDelete.c_str());
                  if (snapIndex.has_value()) {
                    files.insert(files.begin() + static_cast<long>(*snapIndex), snapEntry);
                    rebuildFilteredFileIndexes();
                    clampSelectorIndex();
                  }
                  TransitionFeedback::show(renderer, tr(STR_DELETE_FAILED));
                }
                TransitionFeedback::ensureMinDisplayElapsed();
                mode = Mode::BROWSE;
                // Block 2: full refresh on delete completion — no ghost
                renderer.requestFullRefresh();
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
    if (entry.path == "/sleep" && !FavoriteImage::canPlacePathInSleep(selectedFilePath)) {
      showMessagePopup(FavoriteImage::limitReachedPopupMessage());
      return;
    }
    StatusPopup::showBlocking(renderer, tr(STR_MOVING_FILE));
    std::string destinationPath;
    if (moveSelectedFileTo(entry.path, &destinationPath)) {
      if (entry.path == "/sleep") {
        SleepActivity::trimSleepFolderToLimit();
      }
      StatusPopup::showConfirmation(renderer, tr(STR_MOVED));
      mode = Mode::BROWSE;
      progressPrefixCache.erase(selectedFilePath);
      if (const auto rawIndex = rawFileIndexForPath(selectedFilePath); rawIndex.has_value()) {
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
    StatusPopup::showBlocking(renderer, tr(STR_OPENING));
    loadFiles();
    clearSearch();
    selectorIndex = 0;
    requestUpdate();
    return;
  }

  // Long press CONFIRM (1s+) on a file — opens file actions while held
  {
    const auto rawIndex = rawFileIndexForListIndex(selectorIndex);
    if (rawIndex.has_value() && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        mappedInput.getHeldTime() >= GO_HOME_MS && isManagedFile(files[*rawIndex])) {
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
      StatusPopup::showBlocking(renderer, tr(STR_LOADING));  // SD scan of the next batch can take seconds
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
      StatusPopup::showBlocking(renderer, tr(STR_OPENING));
      loadFiles();
      selectorIndex = 0;
      requestCleanRefresh();
      requestUpdate();
    } else if (isManagedFile(selectedEntry)) {
      if (isImageFile(selectedEntry)) {
        enterImageView(selectedPath);
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
      StatusPopup::showBlocking(renderer, tr(STR_OPENING));
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

  if (mode == Mode::IMAGE_VIEW) {
    renderImageView();
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
  const std::string pathLabel = renderer.truncatedText(SMALL_FONT_ID, basepath.c_str(), pathWidth);
  renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, pathY, pathLabel.c_str());

  const int listTop = pathY + renderer.getLineHeight(SMALL_FONT_ID) + 2;
  const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int rowGap = 1;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int rowPadY = 2;
  const int listX = 0;
  const int listW = pageWidth;
  const int textX = listX + metrics.contentSidePadding;
  const int textW = listW - metrics.contentSidePadding * 2 - 3;

  // A3 paint-then-load placeholder: SD scan deferred to next loop tick.
  // Draw a centered "Loading library…" indicator in the list area so the
  // first frame is meaningful instead of an empty list.
  if (pendingLibraryLoad) {
    const int loadingFont = UI_10_FONT_ID;
    const char* msg = tr(STR_LOADING_LIBRARY);
    const int msgW = renderer.getTextWidth(loadingFont, msg);
    const int msgH = renderer.getLineHeight(loadingFont);
    const int msgX = listX + (listW - msgW) / 2;
    const int msgY = listTop + (listHeight - msgH) / 2;
    renderer.drawText(loadingFont, msgX, msgY, msg);
    return;
  }

  clampSelectorIndex();
  const int selected = static_cast<int>(selectorIndex);
  const int totalRows = static_cast<int>(totalListCount());

  int startIndex = selected;
  int usedHeight = 0;
  for (int i = selected; i >= 0; --i) {
    const auto bwLines = wrapTextToWidth(renderer, UI_10_FONT_ID, getRowTextForListIndex(i), textW);
    const int bwHeight = static_cast<int>(bwLines.size()) * lineHeight + rowPadY * 2;
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
    const auto wrappedLines = wrapTextToWidth(renderer, UI_10_FONT_ID, getRowTextForListIndex(i), textW);
    const int rowHeight = static_cast<int>(wrappedLines.size()) * lineHeight + rowPadY * 2;

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
    const int markerY = std::max(listTop, std::min(listTop + listHeight - markerLineHeight, y - markerLineHeight));
    renderer.drawText(markerFont, markerX, markerY, bottomMarker);
  }

  // Help text
  const auto selectedRawIndex = rawFileIndexForListIndex(selectorIndex);
  const bool hasSelectedFile = selectedRawIndex.has_value() && isManagedFile(files[*selectedRawIndex]);
  const bool hasSelectedImage = selectedRawIndex.has_value() && isImageFile(files[*selectedRawIndex]);
  const char* confirmLabel = tr(STR_OPEN);
  if (isSearchActionRow(selectorIndex)) {
    confirmLabel = tr(STR_SEARCH_BUTTON);
  } else if (isClearSearchRow(selectorIndex)) {
    confirmLabel = tr(STR_CLEAR_BUTTON);
  } else if (hasSelectedFile) {
    confirmLabel = hasSelectedImage ? tr(STR_VIEW_HOLD) : tr(STR_OPEN_HOLD);
  }
  const char* backLabel = basepath == "/" ? tr(STR_HOME) : tr(STR_BACK_HOLD);
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (messagePopupOpen) {
    GUI.drawPopup(renderer, messagePopupText.c_str());
  }

  displayFrame();
}

void MyLibraryActivity::renderImageView() {
  if (isPxcFile(selectedFilePath)) {
    renderPxcImageView();
    return;
  }
  renderBmpImageView();
}

void MyLibraryActivity::renderBmpImageView() {
  renderer.clearScreen();

  FsFile file;
  if (!Storage.openFileForRead("LIB", selectedFilePath, file)) {
    renderer.drawCenteredText(UI_12_FONT_ID, 80, tr(STR_FAILED_OPEN_BMP));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    displayFrame();
    return;
  }

  Bitmap bitmap(file, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    renderer.drawCenteredText(UI_12_FONT_ID, 80, tr(STR_INVALID_BMP));
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

  // Post-load render (returning from menu, popup toggle, etc.): draw
  // buttons and refresh fast. Image was already loaded once, so skip the
  // slow HALF_REFRESH + grayscale pipeline.
  if (imageViewFullyLoaded) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_ACTIONS_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (messagePopupOpen) {
      GUI.drawPopup(renderer, messagePopupText.c_str());
    }
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    nextRefreshMode = HalDisplay::FAST_REFRESH;
    return;
  }

  // Initial load: hide button hints until the image is fully rendered —
  // the HALF_REFRESH below blocks for ~1.7s during which button presses
  // wouldn't be processed. Showing hints now would invite taps that go
  // nowhere.
  if (messagePopupOpen) {
    GUI.drawPopup(renderer, messagePopupText.c_str());
  }

  // HALF_REFRESH (~1.7s) for BW pass — faster than FULL_REFRESH (~3s);
  // residual ghost cleaned by the grayscale pass below (when present).
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  nextRefreshMode = HalDisplay::FAST_REFRESH;

  if (hasGreyscale) {
    // Allow abort before the ~1.5s grayscale pass if the user is already
    // pressing a button. Mark the image as loaded so the next render draws
    // the button hints via the fast post-load path.
    gpio.update();
    if (gpio.wasAnyPressed()) {
      imageViewFullyLoaded = true;
      requestUpdate();
      return;
    }

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

  // Image fully loaded — mark so next frame renders button hints via the
  // fast post-load path above.
  imageViewFullyLoaded = true;
  requestUpdate();
}

void MyLibraryActivity::renderPxcImageView() {
  // Post-load render (returning from menu, popup toggle, etc.): redraw
  // hints + popup with FAST_REFRESH. The grayscale image was already
  // composited by the differential pass below; FAST refresh only drives
  // pixels that differ from the BW base captured during initial load.
  if (imageViewFullyLoaded) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_ACTIONS_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (messagePopupOpen) {
      GUI.drawPopup(renderer, messagePopupText.c_str());
    }
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    nextRefreshMode = HalDisplay::FAST_REFRESH;
    return;
  }

  // Initial PXC viewer load — same strategy as the BMP viewer:
  //   1. BW pass: stream PXC as 1-bit (pv < 3 → black, pv == 3 → white) into
  //      the BW frameBuffer; overlay button hints; HALF_REFRESH. Panel now
  //      shows a stark BW silhouette of the image plus the hints.
  //   2. storeBwBuffer to preserve the BW base.
  //   3. Differential grayscale overlay: re-streams the PXC twice (LSB plane
  //      + MSB plane) and pushes the composite. Differential's encoding
  //      identifies pure-black and pure-white as (0,0) and lets the prior
  //      panel state — set by step 1 — disambiguate them, so stark images
  //      come out correctly without Factory mode's pre-flash wiping the
  //      button hints.
  //   4. restoreBwBuffer (also calls cleanupGrayscaleBuffers, syncing RED
  //      RAM back to the BW base for future FAST_REFRESH transitions).
  renderer.clearScreen();

  if (!PxcRenderer::streamPxcAsBw(renderer, selectedFilePath)) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 80, tr(STR_INVALID_BMP));
    const auto errLabels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, errLabels.btn1, errLabels.btn2, errLabels.btn3, errLabels.btn4);
    displayFrame();
    imageViewFullyLoaded = true;
    return;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_ACTIONS_BUTTON), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (messagePopupOpen) {
    GUI.drawPopup(renderer, messagePopupText.c_str());
  }
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  nextRefreshMode = HalDisplay::FAST_REFRESH;

  if (renderer.storeBwBuffer()) {
    PxcRenderer::renderPxc(renderer, selectedFilePath, GfxRenderer::GrayscaleMode::Differential);
    renderer.restoreBwBuffer();
  }

  imageViewFullyLoaded = true;
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
  renderer.drawCenteredText(UI_12_FONT_ID, popupY + 10, tr(STR_FILE_ACTIONS_TITLE), true, EpdFontFamily::REGULAR);

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
