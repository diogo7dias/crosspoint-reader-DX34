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

bool WallpaperPlaylistV2::nameIsInBuffer(const std::string& name) const {
  if (buffer_.empty() || name.empty()) return false;
  // Match "name\n" at position 0, or "\nname\n" anywhere. \n separators bound
  // the search so prefix-collisions are not misreported (e.g. "a.bmp" vs "ab.bmp").
  const size_t nlen = name.size();
  if (buffer_.size() > nlen && buffer_.compare(0, nlen, name) == 0 && buffer_[nlen] == '\n') return true;
  // Search for "\nname\n" — caller-side concat into a temporary needle.
  std::string needle;
  needle.reserve(nlen + 2);
  needle.push_back('\n');
  needle.append(name);
  needle.push_back('\n');
  return buffer_.find(needle) != std::string::npos;
}

void WallpaperPlaylistV2::reconcile() {
  if (!deps_.fs) return;
  if (!ensureLoaded()) return;
  if (!dirty_) return;

  // Streaming walk: only retain NEW files (those not already in buffer_).
  // Heap cost is proportional to the delta (typically 0-3 entries on a normal
  // session), not the full /sleep listing. This is the critical fix for the
  // bad_alloc that hit boot/home-route reconcile when /sleep had ~500 entries
  // (fragmented heap could not fit the transient ~14 KB vector<SleepBmpEntry>).
  std::vector<SleepBmpEntry> newFiles;
  newFiles.reserve(8);  // typical delta upper bound; grows if exceeded
  size_t diskCount = 0;
  size_t favCount = 0;

  deps_.fs->walkSleepBmps([&](const std::string& name, uint32_t mtime) {
    ++diskCount;
    if (deps_.isFavorite && deps_.isFavorite(makeSleepPath(name))) ++favCount;
    if (!nameIsInBuffer(name)) newFiles.push_back({name, mtime});
  });

  // Trim path. Only enters here if /sleep is over the cap — gates the heavy
  // full-listing materialization on a count-only streaming pass first.
  bool capBlocked = false;
  uint16_t moved = 0;
  if (diskCount > kSleepFolderCap) {
    std::vector<SleepBmpEntry> all = deps_.fs->listSleepBmpsWithMtime(kSleepFolderCap + 64);
    moved = trimToCap(all, capBlocked);
    if (capBlocked && deps_.onFavoritesCapBlocked) deps_.onFavoritesCapBlocked();
    if (moved > 0 && deps_.onTrimMoved) deps_.onTrimMoved(moved);
    // Re-derive newFiles after trim — some new arrivals may have been pushed
    // to /sleep pause if the cap was favorites-saturated.
    newFiles.clear();
    for (auto& e : all) {
      if (!nameIsInBuffer(e.name)) newFiles.push_back(std::move(e));
    }
  }

  if (newFiles.empty()) {
    dirty_ = false;
    return;
  }

  // Sort new files by mtime ascending (oldest first) so multi-drop preserves
  // upload order in the queue.
  std::sort(newFiles.begin(), newFiles.end(),
            [](const SleepBmpEntry& a, const SleepBmpEntry& b) { return a.mtime < b.mtime; });

  // Splice insertion: build a single contiguous payload and inject at cursor_.
  // buffer_.insert may reallocate (transient peak = old + insertion size), but
  // that is one allocation, not 500. Cursor stays at byte position of first
  // new file — next advance() returns it.
  std::string insertion;
  size_t insLen = 0;
  for (const auto& nf : newFiles) insLen += nf.name.size() + 1;
  insertion.reserve(insLen);
  for (const auto& nf : newFiles) {
    insertion.append(nf.name);
    insertion.push_back('\n');
  }

  // If buffer is empty (boot first reconcile, no prior shuffle), build a full
  // shuffled lap from disk via reshuffle. Otherwise splice at cursor.
  if (buffer_.empty()) {
    // Defer to reshuffle which uses listSleepBmps (vector<string>, smaller and
    // gated by the user's normal sleep heap budget).
    reshuffle();
  } else {
    if (cursor_ > buffer_.size()) cursor_ = 0;
    buffer_.insert(cursor_, insertion);
    saveToDisk();
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
