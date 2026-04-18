/**
 * @file WallpaperPlaylist.h
 * @brief Sleep wallpaper playlist module (RFC #22).
 *
 * Owns Small/Large strategy selection with hysteresis, streaming SD scans,
 * playlist advance, reshuffle, and trim-to-limit. State lives in APP_STATE
 * (pointers injected via Deps — zero state.json schema change). All SD and
 * persistence calls are injected, enabling host-side unit tests.
 *
 * Call sites migrate under #if SLEEP_V2 (debug env only until soak-tested).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace crosspoint {
namespace sleep {

class ISleepFs;

// State size at which strategy switches. Hysteresis prevents flap near 200.
constexpr size_t kPlaylistMaxPersist = 200;
constexpr size_t kSmallToLargeThreshold = 210;  // cross up
constexpr size_t kLargeToSmallThreshold = 190;  // cross down

enum class StrategyKind : uint8_t { Small, Large };

struct TrimResult {
  bool movedAny = false;
  size_t favoriteCount = 0;
  size_t overflowMoved = 0;
};

class WallpaperPlaylist {
 public:
  struct Deps {
    ISleepFs* fs = nullptr;

    // State slots — module never copies, only reads/writes through pointers.
    // Production wires to APP_STATE fields; tests wire to a local struct.
    std::vector<std::string>* playlist = nullptr;      // APP_STATE.sleepImagePlaylist
    std::string* lastShownFilename = nullptr;           // APP_STATE.lastShownSleepFilename
    uint8_t* cursor = nullptr;                          // APP_STATE.lastSleepImage (0 = not yet shown, 1 = shown)
    std::string* lastRenderedPath = nullptr;            // APP_STATE.lastSleepWallpaperPath

    // Persistence. Defaults wire to APP_STATE.saveToFile in production.
    std::function<bool()> saveState;

    // RNG. Defaults to Arduino random() in production. Tests inject seeded fn.
    std::function<long(long)> randomFn;

    // Favorite predicate (delegates to FavoriteBmp in production).
    // Argument is a full /sleep/<filename> path.
    std::function<bool(const std::string&)> isFavorite;

    // Called after a file is renamed during trim (for FavoriteBmp path updates).
    std::function<void(const std::string& /*from*/, const std::string& /*to*/)> onPathRenamed;

    // Called by trimToLimit before moving overflow files (activity shows popup).
    std::function<void()> onBeforeTrimMove;
  };

  static WallpaperPlaylist& instance();

  void setDeps(const Deps&);
  const Deps& deps() const { return deps_; }

  // Replaces global sleepFolderDirty flag.
  void markFolderDirty() { dirty_ = true; }
  bool dirty() const { return dirty_; }

  // Resolve dirty state: trim overflow + resync playlist against disk. No-op
  // when !dirty. Safe to call repeatedly.
  void reconcile();

  // Advance to next wallpaper. Returns basename (e.g. "my.bmp") without
  // /sleep/ prefix. Empty return → no BMPs on disk.
  // Persists cursor + lastShownFilename internally.
  std::string advance();

  // Shuffle (Small) or pick random start (Large). Returns false if empty.
  bool reshuffle();

  // Post-render: remember the actually-rendered path (for paused mode) and
  // optionally update lastShownFilename.
  void rememberRendered(const std::string& fullPath, const std::string& filename = "");
  void clearRenderedPath();

  // Trim /sleep to kPlaylistMaxPersist, preserving favorites. Overflow moves
  // to /sleep pause. Updates cachedFavoriteCount.
  TrimResult trimToLimit();

  size_t cachedFavoriteCount() const { return cachedFavoriteCount_; }
  StrategyKind currentStrategy() const { return strategy_; }

  // Reset singleton-owned state (tests only).
  void resetForTest();

 private:
  WallpaperPlaylist() = default;

  // Strategy helpers — operate via deps_.
  StrategyKind pickStrategy(size_t fileCount) const;
  std::string advanceSmall();
  std::string advanceLarge();
  bool resyncSmallPlaylist(const std::vector<std::string>& files);
  void migrateToLarge();                              // drop playlist, keep lastShownFilename
  void migrateToSmall(const std::vector<std::string>& files);  // build + seek cursor

  Deps deps_;
  // dirty_ is set explicitly by callers: markFolderDirty() during
  // file-transfer and on boot. advance() does not auto-reconcile — reconcile
  // is a separate, explicit step run by the lifecycle (boot / onGoHome).
  bool dirty_ = false;
  StrategyKind strategy_ = StrategyKind::Small;
  size_t cachedFavoriteCount_ = 0;
};

}  // namespace sleep
}  // namespace crosspoint
