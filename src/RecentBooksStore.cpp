#include "RecentBooksStore.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>
#include <Xtc.h>

#include <algorithm>
#include <cctype>

#include "CrossPointState.h"
#include "Paths.h"
#include "util/StringUtils.h"

namespace {
constexpr uint8_t RECENT_BOOKS_FILE_VERSION = 3;
constexpr char RECENT_BOOKS_FILE_BIN[] = "/.crosspoint/recent.bin";
constexpr char RECENT_BOOKS_FILE_JSON[] = "/.crosspoint/recent.json";
constexpr char RECENT_BOOKS_FILE_BAK[] = "/.crosspoint/recent.bin.bak";
constexpr int MAX_RECENT_BOOKS = 100;

std::string normalizeRecentPath(std::string path) {
  if (path.empty()) {
    return path;
  }

  std::replace(path.begin(), path.end(), '\\', '/');

  std::string normalized = FsHelpers::normalisePath(path);
  if (normalized.empty()) {
    return "/";
  }
  if (normalized.front() != '/') {
    normalized.insert(normalized.begin(), '/');
  }
  while (normalized.size() > 1 && normalized.back() == '/') {
    normalized.pop_back();
  }
  return normalized;
}

std::string makeRecentPathKey(const std::string& rawPath) {
  std::string key = normalizeRecentPath(rawPath);
  std::transform(key.begin(), key.end(), key.begin(),
                 [](const char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
  return key;
}

void dedupeRecentBooks(std::vector<RecentBook>& books) {
  // In-place dedupe: O(n²) but avoids allocating a full-size temp vector +
  // unordered_set (~45 KB spike for 100 books). n ≤ 100 so this is fast.
  for (size_t i = 0; i < books.size(); ++i) {
    books[i].path = normalizeRecentPath(books[i].path);
    if (books[i].path.empty()) {
      books.erase(books.begin() + static_cast<long>(i));
      --i;
      continue;
    }
    const std::string key = makeRecentPathKey(books[i].path);
    for (size_t j = i + 1; j < books.size();) {
      if (makeRecentPathKey(normalizeRecentPath(books[j].path)) == key) {
        books.erase(books.begin() + static_cast<long>(j));
      } else {
        ++j;
      }
    }
  }

  if (books.size() > MAX_RECENT_BOOKS) {
    books.resize(MAX_RECENT_BOOKS);
  }
}
}  // namespace

RecentBooksStore RecentBooksStore::instance;

void RecentBooksStore::addBook(const std::string& path, const std::string& title, const std::string& author,
                               const std::string& coverBmpPath) {
  const std::string normalizedPath = normalizeRecentPath(path);
  const std::string normalizedKey = makeRecentPathKey(normalizedPath);
  if (normalizedPath.empty()) {
    return;
  }

  // Preserve per-book preferences (e.g. Bold Swap) when the book is re-added:
  // addBook is called every time a book is opened, and erasing-then-reinserting
  // would otherwise reset any per-book toggles the user set earlier.
  uint8_t preservedBoldSwap = 0;

  // Remove existing entry if present
  for (auto it = recentBooks.begin(); it != recentBooks.end();) {
    if (makeRecentPathKey(it->path) == normalizedKey) {
      preservedBoldSwap = it->boldSwap;
      it = recentBooks.erase(it);
    } else {
      ++it;
    }
  }

  // Add to front
  RecentBook entry{normalizedPath, title, author, coverBmpPath};
  entry.boldSwap = preservedBoldSwap;
  recentBooks.insert(recentBooks.begin(), entry);

  dedupeRecentBooks(recentBooks);

  saveToFile();
}

void RecentBooksStore::updateBook(const std::string& path, const std::string& title, const std::string& author,
                                  const std::string& coverBmpPath) {
  const std::string normalizedPath = normalizeRecentPath(path);
  const std::string normalizedKey = makeRecentPathKey(normalizedPath);
  auto it = std::find_if(recentBooks.begin(), recentBooks.end(),
                         [&](const RecentBook& book) { return makeRecentPathKey(book.path) == normalizedKey; });
  if (it != recentBooks.end()) {
    RecentBook& book = *it;
    book.path = normalizedPath;
    book.title = title;
    book.author = author;
    book.coverBmpPath = coverBmpPath;
    dedupeRecentBooks(recentBooks);
    saveToFile();
  }
}

void RecentBooksStore::setPercent(const std::string& path, int percent) {
  const std::string normalizedKey = makeRecentPathKey(normalizeRecentPath(path));
  auto it = std::find_if(recentBooks.begin(), recentBooks.end(),
                         [&](const RecentBook& book) { return makeRecentPathKey(book.path) == normalizedKey; });
  if (it == recentBooks.end()) return;
  // Clamp to int8_t range; -1 means unknown, 0-100 are valid percents.
  const int clamped = (percent < 0) ? -1 : (percent > 100 ? 100 : percent);
  if (it->percent == clamped) return;  // No change, skip the disk write.
  it->percent = static_cast<int8_t>(clamped);
  saveToFile();
}

void RecentBooksStore::removeBook(const std::string& path) {
  const std::string normalizedPath = normalizeRecentPath(path);
  const std::string normalizedKey = makeRecentPathKey(normalizedPath);
  bool removed = false;
  for (auto it = recentBooks.begin(); it != recentBooks.end();) {
    if (makeRecentPathKey(it->path) == normalizedKey) {
      it = recentBooks.erase(it);
      removed = true;
    } else {
      ++it;
    }
  }
  if (removed) {
    dedupeRecentBooks(recentBooks);
    saveToFile();
  }
}

std::string RecentBooksStore::moveBookToRecents(const std::string& bookPath) {
  const std::string normalized = normalizeRecentPath(bookPath);
  if (normalized.empty()) {
    return {};
  }

  // Already in /recents/ — nothing to do
  const std::string lower = makeRecentPathKey(normalized);
  if (lower.rfind("/recents/", 0) == 0) {
    return {};
  }

  // Ensure /recents folder exists
  Storage.mkdir("/recents");

  // Extract filename
  const auto slashPos = normalized.find_last_of('/');
  const std::string filename = (slashPos == std::string::npos) ? normalized : normalized.substr(slashPos + 1);
  const std::string destination = "/recents/" + filename;

  // Skip if destination already exists (name collision)
  if (Storage.exists(destination.c_str())) {
    return {};
  }

  // Build QUOTES sidecar path: strip extension + _QUOTES.txt
  std::string quotesOld;
  std::string quotesNew;
  {
    const auto dotPos = normalized.rfind('.');
    const std::string basePath = (dotPos != std::string::npos) ? normalized.substr(0, dotPos) : normalized;
    quotesOld = basePath + "_QUOTES.txt";

    const auto dotDst = destination.rfind('.');
    const std::string baseDst = (dotDst != std::string::npos) ? destination.substr(0, dotDst) : destination;
    quotesNew = baseDst + "_QUOTES.txt";
  }

  // Move the book file (rename is instant on same filesystem)
  if (!Storage.rename(normalized.c_str(), destination.c_str())) {
    LOG_ERR("RBS", "Failed to move book to recents: %s -> %s", normalized.c_str(), destination.c_str());
    return {};
  }
  LOG_INF("RBS", "Moved book to recents: %s -> %s", normalized.c_str(), destination.c_str());

  // Move QUOTES sidecar if it exists
  if (Storage.exists(quotesOld.c_str())) {
    if (Storage.rename(quotesOld.c_str(), quotesNew.c_str())) {
      LOG_DBG("RBS", "Moved QUOTES sidecar: %s -> %s", quotesOld.c_str(), quotesNew.c_str());
    } else {
      LOG_ERR("RBS", "Failed to move QUOTES sidecar: %s", quotesOld.c_str());
    }
  }

  // Update recents store path
  removeBook(normalized);
  RecentBook data = getDataFromBook(destination);
  addBook(destination, data.title, data.author, data.coverBmpPath);

  // Update APP_STATE if it points to the old path
  if (APP_STATE.openEpubPath == normalized) {
    APP_STATE.openEpubPath = destination;
    APP_STATE.saveToFile();
  }

  return destination;
}

bool RecentBooksStore::saveToFile() const {
  Storage.mkdir(Paths::kDataDir);
  const bool ok = JsonSettingsIO::saveRecentBooks(*this, RECENT_BOOKS_FILE_JSON);
  if (!ok) {
    LOG_ERR("RBS", "Failed to save recent books to %s", RECENT_BOOKS_FILE_JSON);
  }
  return ok;
}

bool RecentBooksStore::getBoldSwap(const std::string& path) const {
  const std::string normalizedKey = makeRecentPathKey(normalizeRecentPath(path));
  if (normalizedKey.empty()) {
    return false;
  }
  for (const auto& book : recentBooks) {
    if (makeRecentPathKey(book.path) == normalizedKey) {
      return book.boldSwap != 0;
    }
  }
  return false;
}

void RecentBooksStore::setBoldSwap(const std::string& path, bool enabled) {
  const std::string normalizedPath = normalizeRecentPath(path);
  const std::string normalizedKey = makeRecentPathKey(normalizedPath);
  if (normalizedPath.empty()) {
    return;
  }
  const uint8_t newValue = enabled ? 1 : 0;
  for (auto& book : recentBooks) {
    if (makeRecentPathKey(book.path) == normalizedKey) {
      if (book.boldSwap == newValue) {
        return;
      }
      book.boldSwap = newValue;
      saveToFile();
      return;
    }
  }

  // In normal flow the reader always calls addBook() before the user can
  // reach the toggle, so setBoldSwap on an unknown path indicates a caller
  // bug. Refuse rather than spawning a ghost recents entry with empty
  // title/author/cover that would leak into the library list.
  LOG_ERR("RBS", "setBoldSwap on unregistered book, ignoring: %s", normalizedPath.c_str());
}

RecentBook RecentBooksStore::getDataFromBook(std::string path) const {
  std::string lastBookFileName = "";
  const size_t lastSlash = path.find_last_of('/');
  if (lastSlash != std::string::npos) {
    lastBookFileName = path.substr(lastSlash + 1);
  }

  LOG_DBG("RBS", "Loading recent book: %s", path.c_str());

  // If epub, try to load the metadata for title/author and cover
  if (StringUtils::checkFileExtension(lastBookFileName, ".epub")) {
    Epub epub(path, Paths::kDataDir);
    epub.load(false, true);
    return RecentBook{path, epub.getTitle(), epub.getAuthor(), epub.getThumbBmpPath()};
  } else if (StringUtils::checkFileExtension(lastBookFileName, ".xtch") ||
             StringUtils::checkFileExtension(lastBookFileName, ".xtc")) {
    // Handle XTC file
    Xtc xtc(path, Paths::kDataDir);
    if (xtc.load()) {
      return RecentBook{path, xtc.getTitle(), xtc.getAuthor(), xtc.getThumbBmpPath()};
    }
  } else if (StringUtils::checkFileExtension(lastBookFileName, ".txt") ||
             StringUtils::checkFileExtension(lastBookFileName, ".md")) {
    return RecentBook{path, lastBookFileName, "", ""};
  }
  return RecentBook{path, "", "", ""};
}

bool RecentBooksStore::loadFromFile() {
  // Try JSON first
  if (!Storage.exists(RECENT_BOOKS_FILE_JSON)) {
    return false;
  }

  String json = JsonSettingsIO::safeReadFile(RECENT_BOOKS_FILE_JSON);
  if (!json.isEmpty()) {
    return JsonSettingsIO::loadRecentBooks(*this, json.c_str());
  }

  // Fall back to binary migration — check upstream path first, then DX34 legacy
  // path
  const char* binPaths[] = {
      RECENT_BOOKS_FILE_BIN,         // "/.crosspoint/recent.bin"
      "/.crosspoint/recent_v2.bin",  // DX34 legacy path before JSON migration
  };
  for (const char* binPath : binPaths) {
    if (!Storage.exists(binPath)) continue;

    FsFile inputFile;
    if (!Storage.openFileForRead("RBS", binPath, inputFile)) continue;

    // Inline the binary parse (same logic as loadFromBinaryFile but
    // path-agnostic)
    uint8_t version;
    serialization::readPod(inputFile, version);
    if (version != 1 && version != 2 && version != 3) {
      LOG_ERR("RBS", "Unknown recent books version %u in %s", version, binPath);
      inputFile.close();
      continue;
    }

    uint8_t count;
    serialization::readPod(inputFile, count);
    recentBooks.clear();
    recentBooks.reserve(count);

    for (uint8_t i = 0; i < count; i++) {
      std::string path, title, author, coverBmpPath;
      serialization::readString(inputFile, path);
      if (version >= 2) {
        serialization::readString(inputFile, title);
        serialization::readString(inputFile, author);
      }
      if (version >= 3) {
        serialization::readString(inputFile, coverBmpPath);
      }
      if (!path.empty()) {
        recentBooks.push_back({normalizeRecentPath(path), title, author, coverBmpPath});
      }
    }
    inputFile.close();

    dedupeRecentBooks(recentBooks);
    LOG_DBG("RBS", "Migrated %d books from %s to recent.json", static_cast<int>(recentBooks.size()), binPath);

    saveToFile();
    Storage.rename(binPath, RECENT_BOOKS_FILE_BAK);
    return true;
  }

  return false;
}

bool RecentBooksStore::loadFromBinaryFile() {
  FsFile inputFile;
  if (!Storage.openFileForRead("RBS", RECENT_BOOKS_FILE_BIN, inputFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version != RECENT_BOOKS_FILE_VERSION) {
    if (version == 1 || version == 2) {
      // Old version, just read paths
      uint8_t count;
      serialization::readPod(inputFile, count);
      recentBooks.clear();
      recentBooks.reserve(count);
      for (uint8_t i = 0; i < count; i++) {
        std::string path;
        serialization::readString(inputFile, path);

        // load book to get missing data
        RecentBook book = getDataFromBook(path);
        if (book.title.empty() && book.author.empty() && version == 2) {
          // Fall back to loading what we can from the store
          std::string title, author;
          serialization::readString(inputFile, title);
          serialization::readString(inputFile, author);
          recentBooks.push_back({path, title, author, ""});
        } else {
          recentBooks.push_back(book);
        }
      }
    } else {
      inputFile.close();
      return false;
    }
  } else {
    uint8_t count;
    serialization::readPod(inputFile, count);

    recentBooks.clear();
    recentBooks.reserve(count);

    for (uint8_t i = 0; i < count; i++) {
      std::string path, title, author, coverBmpPath;
      serialization::readString(inputFile, path);
      serialization::readString(inputFile, title);
      serialization::readString(inputFile, author);
      serialization::readString(inputFile, coverBmpPath);
      recentBooks.push_back({normalizeRecentPath(path), title, author, coverBmpPath});
    }
  }

  dedupeRecentBooks(recentBooks);
  inputFile.close();
  LOG_DBG("RBS", "Recent books loaded from binary file (%d entries)", static_cast<int>(recentBooks.size()));
  return true;
}
