#include "WallpaperPlaylist.h"

#include <algorithm>
#include <cstring>

#include "SleepFs.h"

namespace crosspoint {
namespace sleep {
namespace {

constexpr size_t kTrimScanCap = kPlaylistMaxPersist + 500;  // matches legacy limit

std::string makeSleepPath(const std::string& filename) {
  if (filename.empty()) return {};
  return std::string("/sleep/") + filename;
}

}  // namespace

WallpaperPlaylist& WallpaperPlaylist::instance() {
  static WallpaperPlaylist inst;
  return inst;
}

void WallpaperPlaylist::setDeps(const Deps& d) { deps_ = d; }

void WallpaperPlaylist::resetForTest() {
  deps_ = Deps{};
  dirty_ = false;
  strategy_ = StrategyKind::Small;
  cachedFavoriteCount_ = 0;
}

StrategyKind WallpaperPlaylist::pickStrategy(size_t fileCount) const {
  // Hysteresis: prevent flap when the folder hovers around 200 files.
  if (strategy_ == StrategyKind::Small && fileCount > kSmallToLargeThreshold) return StrategyKind::Large;
  if (strategy_ == StrategyKind::Large && fileCount < kLargeToSmallThreshold) return StrategyKind::Small;
  return strategy_;
}

void WallpaperPlaylist::reconcile() {
  if (!deps_.fs) return;
  if (!dirty_) return;

  trimToLimit();

  // Reassess strategy with current count. trimToLimit already scanned; pick a
  // fresh count to avoid coupling strategy logic to trim internals.
  const size_t count = deps_.fs->countSleepBmps(kTrimScanCap);
  const StrategyKind desired = pickStrategy(count);
  if (desired != strategy_) {
    if (desired == StrategyKind::Large) {
      migrateToLarge();
    } else {
      migrateToSmall(deps_.fs->listSleepBmps(kPlaylistMaxPersist));
    }
    strategy_ = desired;
  }

  dirty_ = false;
}

std::string WallpaperPlaylist::advance() {
  if (!deps_.fs) return {};

  // Reassessment path — the folder can change
  // size without markFolderDirty (favorites toggle, user deletes). Cheap
  // recheck on each advance keeps strategy coherent without disk burst.
  const size_t count = deps_.fs->countSleepBmps(kTrimScanCap);
  if (count == 0) return {};
  const StrategyKind desired = pickStrategy(count);
  if (desired != strategy_) {
    if (desired == StrategyKind::Large) migrateToLarge();
    else migrateToSmall(deps_.fs->listSleepBmps(kPlaylistMaxPersist));
    strategy_ = desired;
  }

  return (strategy_ == StrategyKind::Large) ? advanceLarge() : advanceSmall();
}

std::string WallpaperPlaylist::advanceSmall() {
  if (!deps_.playlist || !deps_.cursor) return {};
  const auto files = deps_.fs->listSleepBmps(kPlaylistMaxPersist);
  if (files.empty()) return {};

  const bool changed = resyncSmallPlaylist(files);
  auto& playlist = *deps_.playlist;
  if (playlist.empty()) return {};

  bool persistNeeded = changed;
  // Advance rotation: playlist[0] is last-shown after first render. Rotate it
  // to the back so the next image is at index 0.
  if (*deps_.cursor != 0 && playlist.size() > 1) {
    const auto first = playlist.front();
    playlist.erase(playlist.begin());
    playlist.push_back(first);
    persistNeeded = true;
  }

  const std::string selected = playlist.front();
  if (*deps_.cursor != 1) {
    *deps_.cursor = 1;
    persistNeeded = true;
  }
  if (deps_.lastShownFilename && *deps_.lastShownFilename != selected) {
    *deps_.lastShownFilename = selected;
    persistNeeded = true;
  }
  if (persistNeeded && deps_.saveState) deps_.saveState();
  return selected;
}

std::string WallpaperPlaylist::advanceLarge() {
  if (!deps_.lastShownFilename || !deps_.cursor) return {};
  auto& lastShown = *deps_.lastShownFilename;

  std::string selected;
  if (lastShown.empty()) {
    selected = deps_.fs->nextSleepBmpAfter("");  // lex-min
  } else if (*deps_.cursor == 0) {
    // Reshuffle set a starting file; show it without advancing.
    selected = lastShown;
    if (!deps_.fs->exists(makeSleepPath(selected))) {
      selected = deps_.fs->nextSleepBmpAfter("");  // fallback
    }
  } else {
    selected = deps_.fs->nextSleepBmpAfter(lastShown);
  }

  if (selected.empty()) return {};

  bool persistNeeded = false;
  if (lastShown != selected) {
    lastShown = selected;
    persistNeeded = true;
  }
  if (*deps_.cursor != 1) {
    *deps_.cursor = 1;
    persistNeeded = true;
  }
  if (persistNeeded && deps_.saveState) deps_.saveState();
  return selected;
}

bool WallpaperPlaylist::resyncSmallPlaylist(const std::vector<std::string>& files) {
  if (!deps_.playlist || !deps_.cursor) return false;
  auto& playlist = *deps_.playlist;
  bool changed = false;

  if (files.empty()) {
    if (!playlist.empty()) {
      playlist.clear();
      changed = true;
    }
    return changed;
  }

  if (playlist.empty()) {
    playlist = files;
    *deps_.cursor = 0;
    return true;
  }

  // Drop entries no longer on disk. `files` is sorted — binary_search is cheap.
  const auto oldSize = playlist.size();
  playlist.erase(
      std::remove_if(playlist.begin(), playlist.end(),
                     [&files](const std::string& e) { return !std::binary_search(files.begin(), files.end(), e); }),
      playlist.end());
  if (playlist.size() != oldSize) changed = true;

  // Find new files not yet in playlist. O(n log n) with pointer sort — no
  // string copies beyond those in `playlist` itself.
  std::vector<const char*> sortedPtrs;
  sortedPtrs.reserve(playlist.size());
  for (const auto& e : playlist) sortedPtrs.push_back(e.c_str());
  std::sort(sortedPtrs.begin(), sortedPtrs.end(), [](const char* a, const char* b) { return std::strcmp(a, b) < 0; });

  std::vector<std::string> newFiles;
  for (const auto& f : files) {
    const bool inPlaylist = std::binary_search(sortedPtrs.begin(), sortedPtrs.end(), f.c_str(),
                                               [](const char* a, const char* b) { return std::strcmp(a, b) < 0; });
    if (!inPlaylist) newFiles.push_back(f);
  }

  if (!newFiles.empty()) {
    // Insert new files right after current head so they show immediately after
    // the in-flight rotation. cursor == 0 → nothing shown yet → insert at 0;
    // cursor == 1 → playlist[0] is last-shown, insert at 1 so it rotates last.
    const size_t insertPos = (*deps_.cursor == 0) ? 0 : std::min<size_t>(1, playlist.size());
    playlist.insert(playlist.begin() + insertPos, newFiles.begin(), newFiles.end());
    changed = true;
  }

  if (playlist.size() > kPlaylistMaxPersist) {
    playlist.erase(playlist.begin() + kPlaylistMaxPersist, playlist.end());
    changed = true;
  }

  if (playlist.empty()) {
    playlist = files;
    *deps_.cursor = 0;
    changed = true;
  }

  return changed;
}

void WallpaperPlaylist::migrateToLarge() {
  if (!deps_.playlist || !deps_.lastShownFilename || !deps_.cursor) return;
  auto& playlist = *deps_.playlist;
  auto& lastShown = *deps_.lastShownFilename;

  // Preserve lastShown: if cursor points at playlist.front() (the current
  // image), remember it so Large advance picks the lex-next after it.
  if (lastShown.empty() && !playlist.empty()) {
    lastShown = playlist.front();
  }
  playlist.clear();
  if (deps_.saveState) deps_.saveState();
}

void WallpaperPlaylist::migrateToSmall(const std::vector<std::string>& files) {
  if (!deps_.playlist || !deps_.lastShownFilename || !deps_.cursor) return;
  auto& playlist = *deps_.playlist;
  auto& lastShown = *deps_.lastShownFilename;

  playlist = files;
  // Shuffle; favorites aren't specially pinned in Small — they sort by name.
  if (deps_.randomFn && playlist.size() > 1) {
    for (size_t i = playlist.size() - 1; i > 0; --i) {
      const auto j = static_cast<size_t>(deps_.randomFn(static_cast<long>(i + 1)));
      std::swap(playlist[i], playlist[j]);
    }
  }

  // Seek cursor: rotate lastShown to front so next advance rotates past it
  // (cursor = 1 keeps existing post-render behavior).
  if (!lastShown.empty()) {
    auto it = std::find(playlist.begin(), playlist.end(), lastShown);
    if (it != playlist.end()) {
      std::rotate(playlist.begin(), it, it + 1);
      *deps_.cursor = 1;
    } else {
      *deps_.cursor = 0;
    }
  } else {
    *deps_.cursor = 0;
  }
  if (deps_.saveState) deps_.saveState();
}

bool WallpaperPlaylist::reshuffle() {
  if (!deps_.fs || !deps_.playlist || !deps_.cursor || !deps_.lastShownFilename) return false;
  const size_t count = deps_.fs->countSleepBmps(kTrimScanCap);
  if (count == 0) {
    if (!deps_.playlist->empty()) {
      deps_.playlist->clear();
      if (deps_.saveState) deps_.saveState();
    }
    return false;
  }

  const StrategyKind desired = pickStrategy(count);
  strategy_ = desired;

  if (desired == StrategyKind::Large) {
    const long idx = deps_.randomFn ? deps_.randomFn(static_cast<long>(count)) : 0;
    const std::string start = deps_.fs->nthSleepBmp(static_cast<size_t>(idx));
    deps_.playlist->clear();
    *deps_.lastShownFilename = start;
    *deps_.cursor = 0;
    if (deps_.saveState) deps_.saveState();
    return !start.empty();
  }

  // Small: build shuffled playlist
  auto files = deps_.fs->listSleepBmps(kPlaylistMaxPersist);
  if (files.empty()) return false;
  if (deps_.randomFn && files.size() > 1) {
    for (size_t i = files.size() - 1; i > 0; --i) {
      const auto j = static_cast<size_t>(deps_.randomFn(static_cast<long>(i + 1)));
      std::swap(files[i], files[j]);
    }
  }
  *deps_.playlist = std::move(files);
  *deps_.cursor = 0;
  deps_.lastShownFilename->clear();
  if (deps_.saveState) deps_.saveState();
  return true;
}

void WallpaperPlaylist::rememberRendered(const std::string& fullPath, const std::string& filename) {
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
  if (changed && deps_.saveState) deps_.saveState();
}

void WallpaperPlaylist::clearRenderedPath() {
  if (!deps_.lastRenderedPath) return;
  if (!deps_.lastRenderedPath->empty()) {
    deps_.lastRenderedPath->clear();
    if (deps_.saveState) deps_.saveState();
  }
}

TrimResult WallpaperPlaylist::trimToLimit() {
  TrimResult result;
  if (!deps_.fs) return result;

  const size_t count = deps_.fs->countSleepBmps(kTrimScanCap);
  if (count <= kPlaylistMaxPersist) {
    // Under limit — count favorites for HomeActivity badge.
    if (deps_.isFavorite) {
      const auto files = deps_.fs->listSleepBmps(kPlaylistMaxPersist);
      size_t favs = 0;
      for (const auto& f : files) {
        if (deps_.isFavorite(makeSleepPath(f))) ++favs;
      }
      cachedFavoriteCount_ = favs;
      result.favoriteCount = favs;
    }
    return result;
  }

  auto files = deps_.fs->listSleepBmps(kTrimScanCap);
  std::vector<std::string> favorites;
  std::vector<std::string> nonFavorites;
  favorites.reserve(files.size());
  nonFavorites.reserve(files.size());
  for (auto& f : files) {
    const bool isFav = deps_.isFavorite && deps_.isFavorite(makeSleepPath(f));
    if (isFav) favorites.push_back(std::move(f));
    else nonFavorites.push_back(std::move(f));
  }
  cachedFavoriteCount_ = favorites.size();
  result.favoriteCount = favorites.size();

  if (favorites.size() > kPlaylistMaxPersist) {
    // Cannot satisfy — all slots taken by favorites. Leave folder alone.
    return result;
  }

  const size_t nonFavBudget = kPlaylistMaxPersist - favorites.size();
  std::vector<std::string> overflow;
  if (nonFavorites.size() > nonFavBudget) {
    overflow.assign(nonFavorites.begin() + nonFavBudget, nonFavorites.end());
  }

  if (overflow.empty()) return result;

  if (deps_.onBeforeTrimMove) deps_.onBeforeTrimMove();

  deps_.fs->mkdir("/sleep pause");
  for (const auto& filename : overflow) {
    const std::string src = makeSleepPath(filename);
    const std::string dst = std::string("/sleep pause/") + filename;
    if (deps_.fs->rename(src, dst)) {
      if (deps_.onPathRenamed) deps_.onPathRenamed(src, dst);
      ++result.overflowMoved;
      result.movedAny = true;
    }
  }
  if (result.movedAny && deps_.saveState) deps_.saveState();
  return result;
}

}  // namespace sleep
}  // namespace crosspoint
