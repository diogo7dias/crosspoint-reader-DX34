#include "WallpaperPlaylistV2.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace crosspoint {
namespace sleep {
namespace v2 {

namespace {

constexpr const char* kSleepDir = "/sleep";
constexpr const char* kSleepPauseDir = "/sleep pause";
constexpr size_t kHeaderMaxLen = 32;

std::string makeSleepPath(const std::string& filename) {
  if (filename.empty()) return {};
  return std::string(kSleepDir) + "/" + filename;
}

std::string makePausePath(const std::string& filename) {
  if (filename.empty()) return {};
  return std::string(kSleepPauseDir) + "/" + filename;
}

// Format: "v1 cursor=N\n<name1>\n<name2>\n..."
std::string serializeOrderFile(const std::string& namesRegion, size_t cursor) {
  char header[kHeaderMaxLen];
  const int n = std::snprintf(header, sizeof(header), "v1 cursor=%zu\n", cursor);
  std::string out;
  out.reserve(static_cast<size_t>(n) + namesRegion.size());
  out.append(header, static_cast<size_t>(n));
  out.append(namesRegion);
  return out;
}

bool parseOrderFile(const std::string& blob, std::string& namesRegion, size_t& cursor) {
  cursor = 0;
  namesRegion.clear();
  if (blob.size() < 3 || blob.compare(0, 3, "v1 ") != 0) return false;
  const auto firstNewline = blob.find('\n');
  if (firstNewline == std::string::npos) return false;
  const std::string header = blob.substr(0, firstNewline);
  const auto eq = header.find("cursor=");
  if (eq == std::string::npos) return false;
  cursor = static_cast<size_t>(std::strtoul(header.c_str() + eq + 7, nullptr, 10));
  namesRegion = blob.substr(firstNewline + 1);
  return true;
}

}  // namespace

WallpaperPlaylistV2& WallpaperPlaylistV2::instance() {
  static WallpaperPlaylistV2 inst;
  return inst;
}

void WallpaperPlaylistV2::setDeps(const Deps& d) {
  deps_ = d;
  loaded_ = false;
  dirty_ = true;
}

void WallpaperPlaylistV2::resetForTest() {
  deps_ = Deps{};
  buffer_.clear();
  cursor_ = 0;
  dirty_ = true;
  loaded_ = false;
}

bool WallpaperPlaylistV2::ensureLoaded() {
  if (loaded_) return true;
  if (!deps_.fileIO) return false;
  loadFromDisk();
  loaded_ = true;
  if (cursor_ > buffer_.size()) cursor_ = 0;
  return true;
}

bool WallpaperPlaylistV2::loadFromDisk() {
  if (!deps_.fileIO) return false;
  const std::string blob = deps_.fileIO->safeRead(deps_.orderFilePath);
  if (blob.empty()) {
    buffer_.clear();
    cursor_ = 0;
    return false;
  }
  std::string names;
  size_t cursor = 0;
  if (!parseOrderFile(blob, names, cursor)) {
    buffer_.clear();
    cursor_ = 0;
    return false;
  }
  buffer_ = std::move(names);
  cursor_ = cursor;
  return true;
}

bool WallpaperPlaylistV2::saveToDisk() const {
  if (!deps_.fileIO) return false;
  return deps_.fileIO->safeWrite(deps_.orderFilePath, serializeOrderFile(buffer_, cursor_));
}

void WallpaperPlaylistV2::writeBuffer(const std::vector<std::string>& names, size_t cursor) {
  size_t total = 0;
  for (const auto& n : names) total += n.size() + 1;
  buffer_.clear();
  buffer_.reserve(total);
  for (const auto& n : names) {
    buffer_.append(n);
    buffer_.push_back('\n');
  }
  cursor_ = cursor;
  saveToDisk();
}

std::vector<std::string> WallpaperPlaylistV2::bufferEntries() const {
  std::vector<std::string> out;
  if (buffer_.empty()) return out;
  size_t start = 0;
  for (size_t i = 0; i < buffer_.size(); ++i) {
    if (buffer_[i] == '\n') {
      if (i > start) out.emplace_back(buffer_.data() + start, i - start);
      start = i + 1;
    }
  }
  return out;
}

size_t WallpaperPlaylistV2::entryCountForTest() const {
  size_t n = 0;
  for (char c : buffer_) {
    if (c == '\n') ++n;
  }
  return n;
}

std::string WallpaperPlaylistV2::peekAtCursor() const {
  if (cursor_ >= buffer_.size()) return {};
  const auto end = buffer_.find('\n', cursor_);
  if (end == std::string::npos) return buffer_.substr(cursor_);
  return buffer_.substr(cursor_, end - cursor_);
}

void WallpaperPlaylistV2::advanceCursor() {
  if (cursor_ >= buffer_.size()) return;
  const auto end = buffer_.find('\n', cursor_);
  if (end == std::string::npos) {
    cursor_ = buffer_.size();
  } else {
    cursor_ = end + 1;
  }
}

uint16_t WallpaperPlaylistV2::trimToCap(std::vector<SleepBmpEntry>& entries, bool& favoritesCapBlocked) {
  favoritesCapBlocked = false;
  if (entries.size() <= kSleepFolderCap) return 0;

  std::vector<SleepBmpEntry> nonFav;
  nonFav.reserve(entries.size());
  std::vector<SleepBmpEntry> favs;
  favs.reserve(entries.size());
  for (auto& e : entries) {
    const bool fav = deps_.isFavorite && deps_.isFavorite(makeSleepPath(e.name));
    if (fav)
      favs.push_back(std::move(e));
    else
      nonFav.push_back(std::move(e));
  }

  const size_t excess = entries.size() - kSleepFolderCap;
  if (favs.size() >= kSleepFolderCap) {
    favoritesCapBlocked = true;
    if (deps_.fs) deps_.fs->mkdir(kSleepPauseDir);
    uint16_t moved = 0;
    for (const auto& e : nonFav) {
      const std::string from = makeSleepPath(e.name);
      const std::string to = makePausePath(e.name);
      if (deps_.fs && deps_.fs->rename(from, to)) {
        if (deps_.onPathRenamed) deps_.onPathRenamed(from, to);
        ++moved;
      }
    }
    entries = std::move(favs);
    return moved;
  }

  std::sort(nonFav.begin(), nonFav.end(),
            [](const SleepBmpEntry& a, const SleepBmpEntry& b) { return a.mtime < b.mtime; });

  if (deps_.fs) deps_.fs->mkdir(kSleepPauseDir);
  uint16_t moved = 0;
  size_t toMove = std::min(excess, nonFav.size());
  for (size_t i = 0; i < toMove; ++i) {
    const std::string from = makeSleepPath(nonFav[i].name);
    const std::string to = makePausePath(nonFav[i].name);
    if (deps_.fs && deps_.fs->rename(from, to)) {
      if (deps_.onPathRenamed) deps_.onPathRenamed(from, to);
      ++moved;
    }
  }

  std::vector<SleepBmpEntry> surviving;
  surviving.reserve(favs.size() + nonFav.size() - toMove);
  for (auto& e : favs) surviving.push_back(std::move(e));
  for (size_t i = toMove; i < nonFav.size(); ++i) surviving.push_back(std::move(nonFav[i]));
  entries = std::move(surviving);
  return moved;
}

void WallpaperPlaylistV2::reconcile() {
  if (!deps_.fs) return;
  if (!ensureLoaded()) return;
  if (!dirty_) return;

  auto entries = deps_.fs->listSleepBmpsWithMtime(kSleepFolderCap + 64);

  bool favoritesCapBlocked = false;
  const uint16_t moved = trimToCap(entries, favoritesCapBlocked);
  if (moved > 0 && deps_.onTrimMoved) deps_.onTrimMoved(moved);
  if (favoritesCapBlocked && deps_.onFavoritesCapBlocked) deps_.onFavoritesCapBlocked();

  std::unordered_set<std::string> diskSet;
  diskSet.reserve(entries.size() * 2);
  for (const auto& e : entries) diskSet.insert(e.name);

  auto current = bufferEntries();
  std::unordered_set<std::string> bufferSet(current.begin(), current.end());

  std::vector<SleepBmpEntry> newFiles;
  for (const auto& e : entries) {
    if (bufferSet.find(e.name) == bufferSet.end()) newFiles.push_back(e);
  }
  std::sort(newFiles.begin(), newFiles.end(),
            [](const SleepBmpEntry& a, const SleepBmpEntry& b) { return a.mtime < b.mtime; });

  bool anyGone = false;
  for (const auto& name : current) {
    if (diskSet.find(name) == diskSet.end()) {
      anyGone = true;
      break;
    }
  }

  if (newFiles.empty() && !anyGone) {
    dirty_ = false;
    return;
  }

  const std::string cursorTarget = peekAtCursor();

  std::vector<std::string> rebuilt;
  rebuilt.reserve(current.size() + newFiles.size());

  std::vector<std::string> preCursor;
  std::vector<std::string> postCursor;
  bool reachedCursor = false;
  for (const auto& name : current) {
    const bool isCursor = (!cursorTarget.empty() && name == cursorTarget);
    if (isCursor) reachedCursor = true;
    if (diskSet.find(name) == diskSet.end()) continue;
    if (!reachedCursor)
      preCursor.push_back(name);
    else
      postCursor.push_back(name);
  }

  for (auto& n : preCursor) rebuilt.push_back(std::move(n));
  size_t newCursorByte = 0;
  for (const auto& n : rebuilt) newCursorByte += n.size() + 1;
  for (auto& nf : newFiles) rebuilt.push_back(nf.name);
  for (auto& n : postCursor) rebuilt.push_back(std::move(n));

  if (current.empty() || cursorTarget.empty()) {
    std::vector<std::string> all;
    all.reserve(rebuilt.size());
    for (auto& n : rebuilt) all.push_back(std::move(n));
    if (deps_.randomFn && all.size() > 1) {
      for (size_t i = all.size() - 1; i > 0; --i) {
        const auto j = static_cast<size_t>(deps_.randomFn(static_cast<long>(i + 1)));
        std::swap(all[i], all[j]);
      }
    }
    writeBuffer(all, 0);
  } else {
    writeBuffer(rebuilt, newCursorByte);
  }

  dirty_ = false;
}

std::string WallpaperPlaylistV2::advance() {
  if (!deps_.fs) return {};
  if (!ensureLoaded()) return {};

  if (buffer_.empty()) {
    if (!reshuffle()) return {};
  }

  for (int skipBudget = 0; skipBudget < 16; ++skipBudget) {
    if (cursor_ >= buffer_.size()) {
      if (!reshuffle()) return {};
    }
    const std::string candidate = peekAtCursor();
    if (candidate.empty()) {
      if (!reshuffle()) return {};
      continue;
    }
    if (deps_.fs->exists(makeSleepPath(candidate))) {
      advanceCursor();
      saveToDisk();
      if (deps_.lastShownFilename && *deps_.lastShownFilename != candidate) {
        *deps_.lastShownFilename = candidate;
        if (deps_.saveAppState) deps_.saveAppState();
      }
      return candidate;
    }
    advanceCursor();
  }
  return {};
}

bool WallpaperPlaylistV2::reshuffle() {
  if (!deps_.fs) return false;
  auto names = deps_.fs->listSleepBmps(kSleepFolderCap);
  if (names.empty()) {
    buffer_.clear();
    cursor_ = 0;
    saveToDisk();
    return false;
  }
  if (deps_.randomFn && names.size() > 1) {
    for (size_t i = names.size() - 1; i > 0; --i) {
      const auto j = static_cast<size_t>(deps_.randomFn(static_cast<long>(i + 1)));
      std::swap(names[i], names[j]);
    }
  }
  writeBuffer(names, 0);
  loaded_ = true;
  return true;
}

void WallpaperPlaylistV2::rememberRendered(const std::string& fullPath, const std::string& filename) {
  if (!deps_.lastRenderedPath) return;
  bool changed = false;
  if (*deps_.lastRenderedPath != fullPath) {
    *deps_.lastRenderedPath = fullPath;
    changed = true;
  }
  if (!filename.empty() && deps_.lastShownFilename && *deps_.lastShownFilename != filename) {
    *deps_.lastShownFilename = filename;
    changed = true;
  }
  if (changed && deps_.saveAppState) deps_.saveAppState();
}

void WallpaperPlaylistV2::clearRenderedPath() {
  if (!deps_.lastRenderedPath) return;
  if (!deps_.lastRenderedPath->empty()) {
    deps_.lastRenderedPath->clear();
    if (deps_.saveAppState) deps_.saveAppState();
  }
}

}  // namespace v2
}  // namespace sleep
}  // namespace crosspoint
