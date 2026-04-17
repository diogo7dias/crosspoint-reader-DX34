#include "BookmarkStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

namespace {
constexpr char kBookmarksFile[] = "/bookmarks.json";
}  // namespace

bool BookmarkStore::load(const std::string& cachePath) {
  bookmarks.clear();
  if (cachePath.empty()) return false;

  const std::string path = cachePath + kBookmarksFile;
  const std::string tmpPath = path + ".tmp";
  FsFile f;
  if (!Storage.openFileForRead("BKM", path, f)) {
    // Try .tmp fallback — save may have been interrupted after write but before rename
    if (!Storage.openFileForRead("BKM", tmpPath, f)) {
      return false;  // No bookmarks yet — not an error
    }
  }

  const auto sz = static_cast<size_t>(f.size());
  if (sz == 0 || sz > 8192) {
    f.close();
    return false;
  }

  std::vector<char> buf(sz + 1);
  const int rd = f.read(buf.data(), sz);
  f.close();
  if (rd != static_cast<int>(sz)) return false;
  buf[sz] = '\0';

  JsonDocument doc;
  if (deserializeJson(doc, buf.data()) != DeserializationError::Ok) {
    LOG_ERR("BKM", "Failed to parse bookmarks: %s", path.c_str());
    return false;
  }

  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject obj : arr) {
    if (static_cast<int>(bookmarks.size()) >= MAX_BOOKMARKS) break;
    Bookmark bm;
    bm.spineIndex = obj["s"] | 0;
    bm.pageNumber = obj["p"] | 0;
    const char* n = obj["n"] | "";
    bm.name = n;
    if (bm.name.size() > MAX_NAME_LENGTH) bm.name.resize(MAX_NAME_LENGTH);
    bookmarks.push_back(bm);
  }

  LOG_DBG("BKM", "Loaded %d bookmarks from %s", count(), path.c_str());
  return true;
}

bool BookmarkStore::save(const std::string& cachePath) const {
  if (cachePath.empty()) return false;

  const std::string path = cachePath + kBookmarksFile;

  if (bookmarks.empty()) {
    Storage.remove(path.c_str());
    return true;
  }

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const auto& bm : bookmarks) {
    JsonObject obj = arr.add<JsonObject>();
    obj["s"] = bm.spineIndex;
    obj["p"] = bm.pageNumber;
    if (!bm.name.empty()) obj["n"] = bm.name;
  }

  // Atomic write: write to .tmp, close, remove original, rename .tmp.
  // Power loss between remove and rename loses bookmarks (FAT32 limitation),
  // but this prevents the much more common partial-write corruption.
  const std::string tmpPath = path + ".tmp";
  if (Storage.exists(tmpPath.c_str())) {
    Storage.remove(tmpPath.c_str());
  }

  FsFile f;
  if (!Storage.openFileForWrite("BKM", tmpPath, f)) {
    LOG_ERR("BKM", "Failed to write bookmarks tmp: %s", tmpPath.c_str());
    return false;
  }

  if (serializeJson(doc, f) == 0) {
    LOG_ERR("BKM", "Failed to serialize bookmarks");
    f.close();
    return false;
  }
  f.flush();
  f.close();

  if (Storage.exists(path.c_str())) {
    Storage.remove(path.c_str());
  }
  if (!Storage.rename(tmpPath.c_str(), path.c_str())) {
    LOG_ERR("BKM", "Failed to promote bookmarks tmp to %s", path.c_str());
    return false;
  }

  LOG_DBG("BKM", "Saved %d bookmarks to %s", count(), path.c_str());
  return true;
}

int BookmarkStore::toggle(int spineIndex, int pageNumber) {
  for (auto it = bookmarks.begin(); it != bookmarks.end(); ++it) {
    if (it->spineIndex == spineIndex && it->pageNumber == pageNumber) {
      bookmarks.erase(it);
      return 0;  // Removed
    }
  }

  if (static_cast<int>(bookmarks.size()) >= MAX_BOOKMARKS) {
    return -1;  // At capacity
  }

  bookmarks.push_back({spineIndex, pageNumber, ""});

  // Sort by spine index, then page number for consistent display order
  std::sort(bookmarks.begin(), bookmarks.end(),
            [](const Bookmark& a, const Bookmark& b) {
              if (a.spineIndex != b.spineIndex) return a.spineIndex < b.spineIndex;
              return a.pageNumber < b.pageNumber;
            });

  return 1;  // Added
}

bool BookmarkStore::rename(int index, const std::string& newName) {
  if (index < 0 || index >= static_cast<int>(bookmarks.size())) return false;
  std::string trimmed = newName;
  if (trimmed.size() > MAX_NAME_LENGTH) trimmed.resize(MAX_NAME_LENGTH);
  bookmarks[index].name = std::move(trimmed);
  return true;
}

bool BookmarkStore::has(int spineIndex, int pageNumber) const {
  for (const auto& bm : bookmarks) {
    if (bm.spineIndex == spineIndex && bm.pageNumber == pageNumber) {
      return true;
    }
  }
  return false;
}

void BookmarkStore::remove(int index) {
  if (index >= 0 && index < static_cast<int>(bookmarks.size())) {
    bookmarks.erase(bookmarks.begin() + index);
  }
}
