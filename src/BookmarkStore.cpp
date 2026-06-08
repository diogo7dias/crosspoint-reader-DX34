#include "BookmarkStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

#include "persist/BackupMirror.h"

namespace {
constexpr char kBookmarksFile[] = "/bookmarks.json";
}  // namespace

namespace {
bool parseBookmarksFile(const std::string& path, std::vector<BookmarkStore::Bookmark>& out) {
  FsFile f;
  if (!Storage.openFileForRead("BKM", path, f)) return false;
  const auto sz = static_cast<size_t>(f.size());
  if (sz == 0 || sz > 8192) {
    f.close();
    return false;
  }
  // Stream-parse straight from the file. ArduinoJson reads the file
  // incrementally, so the bookmark payload is never duplicated into a
  // separate full-size buffer (this used to slurp the whole file into a
  // std::vector<char> that lived alongside the parsed doc). The size guard
  // above still bounds the input.
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err != DeserializationError::Ok) return false;

  out.clear();
  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject obj : arr) {
    if (static_cast<int>(out.size()) >= BookmarkStore::MAX_BOOKMARKS) break;
    BookmarkStore::Bookmark bm;
    bm.spineIndex = obj["s"] | 0;
    bm.pageNumber = obj["p"] | 0;
    const char* n = obj["n"] | "";
    bm.name = n;
    if (bm.name.size() > BookmarkStore::MAX_NAME_LENGTH) bm.name.resize(BookmarkStore::MAX_NAME_LENGTH);
    out.push_back(bm);
  }
  return true;
}
}  // namespace

bool BookmarkStore::load(const std::string& cachePath) {
  bookmarks.clear();
  if (cachePath.empty()) return false;

  const std::string path = cachePath + kBookmarksFile;
  const std::string tmpPath = path + ".tmp";
  const std::string bakPath = path + ".bak";

  // Try primary, then .tmp (save-interrupted), then .bak (2-layer rollback).
  const std::string sources[] = {path, tmpPath, bakPath};
  const char* labels[] = {"primary", ".tmp", ".bak"};
  for (size_t i = 0; i < 3; i++) {
    if (parseBookmarksFile(sources[i], bookmarks)) {
      if (i == 0) {
        LOG_DBG("BKM", "Loaded %d bookmarks from %s", count(), sources[i].c_str());
      } else {
        LOG_INF("BKM", "Loaded %d bookmarks from %s (%s recovery)", count(), sources[i].c_str(), labels[i]);
      }
      return true;
    }
  }

  // Last resort: try /.crosspoint/backups/<hash>_bookmarks.json mirror.
  const std::string flatName = backup::flatNameForCacheFile(cachePath, "bookmarks.json");
  if (backup::restoreFromMirror(flatName, path)) {
    if (parseBookmarksFile(path, bookmarks)) {
      LOG_INF("BKM", "Recovered %d bookmarks from mirror %s", count(), flatName.c_str());
      return true;
    }
  }

  return false;
}

bool BookmarkStore::save(const std::string& cachePath) const {
  if (cachePath.empty()) return false;

  const std::string path = cachePath + kBookmarksFile;
  const std::string tmpPath = path + ".tmp";
  const std::string bakPath = path + ".bak";

  if (bookmarks.empty()) {
    // User cleared all bookmarks — remove primary and .bak so nothing resurrects them
    Storage.remove(path.c_str());
    Storage.remove(bakPath.c_str());
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

  // 2-layer atomic write: write .tmp → rotate primary → .bak → promote .tmp → primary.
  // Prior .bak becomes the undo slot; on promote failure we restore from it.
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
    Storage.remove(tmpPath.c_str());
    return false;
  }
  f.flush();
  f.close();

  if (Storage.exists(bakPath.c_str())) {
    if (!Storage.remove(bakPath.c_str())) {
      LOG_ERR("BKM", "Failed to remove stale bak %s", bakPath.c_str());
    }
  }
  if (Storage.exists(path.c_str())) {
    if (!Storage.rename(path.c_str(), bakPath.c_str())) {
      LOG_ERR("BKM", "Failed to rotate %s -> %s", path.c_str(), bakPath.c_str());
      Storage.remove(tmpPath.c_str());
      return false;
    }
  }
  if (!Storage.rename(tmpPath.c_str(), path.c_str())) {
    LOG_ERR("BKM", "Failed to promote bookmarks tmp to %s", path.c_str());
    if (Storage.exists(bakPath.c_str())) {
      if (Storage.rename(bakPath.c_str(), path.c_str())) {
        LOG_INF("BKM", "Restored bookmarks from .bak after promote failure");
      } else {
        LOG_ERR("BKM", "Failed to restore bookmarks from .bak");
      }
    }
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
  std::sort(bookmarks.begin(), bookmarks.end(), [](const Bookmark& a, const Bookmark& b) {
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
