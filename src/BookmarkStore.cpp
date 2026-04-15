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
  FsFile f;
  if (!Storage.openFileForRead("BKM", path, f)) {
    return false;  // No bookmarks yet — not an error
  }

  const auto sz = static_cast<size_t>(f.size());
  if (sz == 0 || sz > 4096) {
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
    bookmarks.push_back(bm);
  }

  LOG_DBG("BKM", "Loaded %d bookmarks from %s", count(), path.c_str());
  return true;
}

bool BookmarkStore::save(const std::string& cachePath) const {
  if (cachePath.empty()) return false;

  const std::string path = cachePath + kBookmarksFile;

  if (bookmarks.empty()) {
    // Remove file if no bookmarks
    Storage.remove(path.c_str());
    return true;
  }

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const auto& bm : bookmarks) {
    JsonObject obj = arr.add<JsonObject>();
    obj["s"] = bm.spineIndex;
    obj["p"] = bm.pageNumber;
  }

  FsFile f;
  if (!Storage.openFileForWrite("BKM", path, f)) {
    LOG_ERR("BKM", "Failed to write bookmarks: %s", path.c_str());
    return false;
  }

  if (serializeJson(doc, f) == 0) {
    LOG_ERR("BKM", "Failed to serialize bookmarks");
    f.close();
    return false;
  }

  f.close();
  LOG_DBG("BKM", "Saved %d bookmarks to %s", count(), path.c_str());
  return true;
}

bool BookmarkStore::toggle(int spineIndex, int pageNumber) {
  for (auto it = bookmarks.begin(); it != bookmarks.end(); ++it) {
    if (it->spineIndex == spineIndex && it->pageNumber == pageNumber) {
      bookmarks.erase(it);
      return false;  // Removed
    }
  }

  if (static_cast<int>(bookmarks.size()) >= MAX_BOOKMARKS) {
    return false;
  }

  bookmarks.push_back({spineIndex, pageNumber});

  // Sort by spine index, then page number for consistent display order
  std::sort(bookmarks.begin(), bookmarks.end(),
            [](const Bookmark& a, const Bookmark& b) {
              if (a.spineIndex != b.spineIndex) return a.spineIndex < b.spineIndex;
              return a.pageNumber < b.pageNumber;
            });

  return true;  // Added
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
