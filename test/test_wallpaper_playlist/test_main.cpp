// Host-side tests for sleep::WallpaperPlaylist (RFC #22).
//
// Run via:  pio test -e test_host -f test_wallpaper_playlist

#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "sleep/SleepFs.h"
#include "sleep/WallpaperPlaylist.h"

using crosspoint::sleep::ISleepFs;
using crosspoint::sleep::kLargeToSmallThreshold;
using crosspoint::sleep::kPlaylistMaxPersist;
using crosspoint::sleep::kSmallToLargeThreshold;
using crosspoint::sleep::StrategyKind;
using crosspoint::sleep::TrimResult;
using crosspoint::sleep::WallpaperPlaylist;

namespace {

// ── FakeSleepFs ─────────────────────────────────────────────────────────────

class FakeSleepFs : public ISleepFs {
 public:
  // Files under /sleep (filenames only, no path).
  std::vector<std::string> sleepFiles;
  // Other existing paths (for exists()).
  std::vector<std::string> otherPaths;
  // Recorded renames + mkdirs.
  std::vector<std::pair<std::string, std::string>> renames;
  std::vector<std::string> mkdirs;
  // Count of full directory scans (countSleepBmps + listSleepBmps).
  size_t scanCount = 0;

  size_t countSleepBmps(size_t scanCap) override {
    ++scanCount;
    size_t n = 0;
    for (const auto& f : sleepFiles) {
      if (isBmp(f)) {
        ++n;
        if (n > scanCap) break;
      }
    }
    return n;
  }

  std::vector<std::string> listSleepBmps(size_t maxEntries) override {
    ++scanCount;
    std::vector<std::string> out;
    for (const auto& f : sleepFiles) {
      if (isBmp(f)) out.push_back(f);
      if (out.size() >= maxEntries) break;
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  std::string nextSleepBmpAfter(const std::string& after) override {
    std::string minName, nextName;
    for (const auto& f : sleepFiles) {
      if (!isBmp(f)) continue;
      if (minName.empty() || f < minName) minName = f;
      if (!after.empty() && f > after) {
        if (nextName.empty() || f < nextName) nextName = f;
      }
    }
    if (!after.empty() && !nextName.empty()) return nextName;
    return minName;
  }

  std::string nthSleepBmp(size_t n) override {
    size_t idx = 0;
    for (const auto& f : sleepFiles) {
      if (!isBmp(f)) continue;
      if (idx == n) return f;
      ++idx;
    }
    return "";
  }

  bool exists(const std::string& path) override {
    if (path.rfind("/sleep/", 0) == 0) {
      const std::string name = path.substr(7);
      for (const auto& f : sleepFiles)
        if (f == name) return true;
    }
    for (const auto& p : otherPaths)
      if (p == path) return true;
    return false;
  }

  bool mkdir(const std::string& path) override {
    mkdirs.push_back(path);
    return true;
  }

  bool rename(const std::string& from, const std::string& to) override {
    renames.emplace_back(from, to);
    if (from.rfind("/sleep/", 0) == 0) {
      const std::string name = from.substr(7);
      sleepFiles.erase(std::remove(sleepFiles.begin(), sleepFiles.end(), name), sleepFiles.end());
    }
    return true;
  }

 private:
  static bool isBmp(const std::string& f) {
    if (f.empty() || f[0] == '.') return false;
    if (f.size() < 4) return false;
    return f.compare(f.size() - 4, 4, ".bmp") == 0;
  }
};

// ── Test fixture ────────────────────────────────────────────────────────────

struct Fixture {
  FakeSleepFs fs;
  std::vector<std::string> playlist;
  std::string lastShownFilename;
  uint8_t cursor = 0;
  std::string lastRenderedPath;
  int saves = 0;
  // Deterministic RNG — returns idx sequence [0, 0, 0, ...] by default. Tests
  // override per case.
  std::vector<long> rngQueue;

  WallpaperPlaylist::Deps deps() {
    WallpaperPlaylist::Deps d;
    d.fs = &fs;
    d.playlist = &playlist;
    d.lastShownFilename = &lastShownFilename;
    d.cursor = &cursor;
    d.lastRenderedPath = &lastRenderedPath;
    d.saveState = [this]() {
      ++saves;
      return true;
    };
    d.randomFn = [this](long mod) -> long {
      if (rngQueue.empty()) return 0;
      const long v = rngQueue.front();
      rngQueue.erase(rngQueue.begin());
      return v % (mod > 0 ? mod : 1);
    };
    d.isFavorite = [](const std::string&) { return false; };
    d.onPathRenamed = [](const std::string&, const std::string&) {};
    d.onBeforeTrimMove = []() {};
    return d;
  }
};

std::vector<std::string> makeBmps(size_t n) {
  std::vector<std::string> out;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    char buf[32];
    // zero-pad so lex order == numeric order
    std::snprintf(buf, sizeof(buf), "f%04zu.bmp", i);
    out.emplace_back(buf);
  }
  return out;
}

void resetModule() { WallpaperPlaylist::instance().resetForTest(); }

}  // namespace

// ── Tests ───────────────────────────────────────────────────────────────────

void setUp() { resetModule(); }
void tearDown() {}

void test_small_empty_folder_advance_returns_empty() {
  Fixture fx;
  auto& wp = WallpaperPlaylist::instance();
  wp.setDeps(fx.deps());
  TEST_ASSERT_EQUAL_STRING("", wp.advance().c_str());
}

void test_small_first_advance_populates_playlist_and_returns_front() {
  Fixture fx;
  fx.fs.sleepFiles = makeBmps(5);
  auto& wp = WallpaperPlaylist::instance();
  wp.setDeps(fx.deps());

  const auto first = wp.advance();
  TEST_ASSERT_EQUAL_STRING("f0000.bmp", first.c_str());  // sorted, cursor=0 → no rotation
  TEST_ASSERT_EQUAL(1, fx.cursor);
  TEST_ASSERT_EQUAL(5, fx.playlist.size());
  TEST_ASSERT_EQUAL_STRING("f0000.bmp", fx.lastShownFilename.c_str());
}

void test_small_second_advance_rotates_front_to_back() {
  Fixture fx;
  fx.fs.sleepFiles = makeBmps(3);
  auto& wp = WallpaperPlaylist::instance();
  wp.setDeps(fx.deps());

  wp.advance();  // f0000
  const auto second = wp.advance();
  TEST_ASSERT_EQUAL_STRING("f0001.bmp", second.c_str());
  TEST_ASSERT_EQUAL(3, fx.playlist.size());
  TEST_ASSERT_EQUAL_STRING("f0001.bmp", fx.playlist.front().c_str());
  TEST_ASSERT_EQUAL_STRING("f0000.bmp", fx.playlist.back().c_str());
}

void test_small_resync_drops_missing_files() {
  Fixture fx;
  fx.playlist = {"a.bmp", "b.bmp", "c.bmp"};
  fx.cursor = 1;
  fx.fs.sleepFiles = {"a.bmp", "c.bmp"};  // b removed
  auto& wp = WallpaperPlaylist::instance();
  wp.setDeps(fx.deps());

  wp.advance();
  // b.bmp should be gone
  const auto it = std::find(fx.playlist.begin(), fx.playlist.end(), std::string("b.bmp"));
  TEST_ASSERT_TRUE(it == fx.playlist.end());
}

void test_small_resync_inserts_new_files_at_cursor() {
  Fixture fx;
  fx.playlist = {"a.bmp"};
  fx.cursor = 1;  // a.bmp was last shown
  fx.fs.sleepFiles = {"a.bmp", "z.bmp"};
  auto& wp = WallpaperPlaylist::instance();
  wp.setDeps(fx.deps());

  wp.advance();
  // Expect z.bmp shown next (inserted at pos 1, then rotation moves a to back).
  TEST_ASSERT_EQUAL_STRING("z.bmp", fx.lastShownFilename.c_str());
}

void test_large_advance_uses_next_after() {
  Fixture fx;
  fx.fs.sleepFiles = makeBmps(kSmallToLargeThreshold + 5);  // 215 files → Large
  auto& wp = WallpaperPlaylist::instance();
  wp.setDeps(fx.deps());

  const auto first = wp.advance();
  TEST_ASSERT_EQUAL_STRING("f0000.bmp", first.c_str());
  TEST_ASSERT_EQUAL(StrategyKind::Large, wp.currentStrategy());
  TEST_ASSERT_TRUE(fx.playlist.empty());  // Large: no playlist

  const auto second = wp.advance();
  TEST_ASSERT_EQUAL_STRING("f0001.bmp", second.c_str());
}

void test_large_advance_wraps_at_end() {
  Fixture fx;
  fx.fs.sleepFiles = makeBmps(kSmallToLargeThreshold + 1);
  fx.lastShownFilename = fx.fs.sleepFiles.back();                       // last alphabetically
  std::sort(fx.lastShownFilename.begin(), fx.lastShownFilename.end());  // no-op, already sorted input
  fx.cursor = 1;
  auto& wp = WallpaperPlaylist::instance();
  wp.setDeps(fx.deps());

  // After last file, next should wrap to lex-min.
  const auto next = wp.advance();
  TEST_ASSERT_EQUAL_STRING("f0000.bmp", next.c_str());
}

void test_hysteresis_195_small_stays_small() {
  Fixture fx;
  fx.fs.sleepFiles = makeBmps(195);  // between 190 and 210 → no switch
  auto& wp = WallpaperPlaylist::instance();
  wp.setDeps(fx.deps());
  wp.advance();
  TEST_ASSERT_EQUAL(StrategyKind::Small, wp.currentStrategy());
}

void test_hysteresis_195_large_stays_large() {
  Fixture fx;
  fx.fs.sleepFiles = makeBmps(195);
  auto& wp = WallpaperPlaylist::instance();
  wp.setDeps(fx.deps());
  // Force Large start by pre-crossing 211.
  fx.fs.sleepFiles = makeBmps(kSmallToLargeThreshold + 1);
  wp.advance();
  TEST_ASSERT_EQUAL(StrategyKind::Large, wp.currentStrategy());
  // Now drop to 195 — Large should persist (> 190 threshold).
  fx.fs.sleepFiles = makeBmps(195);
  wp.advance();
  TEST_ASSERT_EQUAL(StrategyKind::Large, wp.currentStrategy());
}

void test_hysteresis_downgrade_below_190() {
  Fixture fx;
  fx.fs.sleepFiles = makeBmps(kSmallToLargeThreshold + 5);
  auto& wp = WallpaperPlaylist::instance();
  wp.setDeps(fx.deps());
  wp.advance();
  TEST_ASSERT_EQUAL(StrategyKind::Large, wp.currentStrategy());

  fx.fs.sleepFiles = makeBmps(kLargeToSmallThreshold - 1);  // 189 → Small
  wp.advance();
  TEST_ASSERT_EQUAL(StrategyKind::Small, wp.currentStrategy());
}

void test_reshuffle_small_sets_playlist() {
  Fixture fx;
  fx.fs.sleepFiles = makeBmps(5);
  auto& wp = WallpaperPlaylist::instance();
  wp.setDeps(fx.deps());

  TEST_ASSERT_TRUE(wp.reshuffle());
  TEST_ASSERT_EQUAL(5, fx.playlist.size());
  TEST_ASSERT_EQUAL(0, fx.cursor);
}

void test_reshuffle_large_sets_lastShown() {
  Fixture fx;
  fx.fs.sleepFiles = makeBmps(kSmallToLargeThreshold + 10);
  fx.rngQueue.push_back(7);  // pick idx 7
  auto& wp = WallpaperPlaylist::instance();
  wp.setDeps(fx.deps());

  TEST_ASSERT_TRUE(wp.reshuffle());
  TEST_ASSERT_EQUAL_STRING("f0007.bmp", fx.lastShownFilename.c_str());
  TEST_ASSERT_EQUAL(0, fx.cursor);
  TEST_ASSERT_TRUE(fx.playlist.empty());
}

void test_trim_moves_overflow_preserves_favorites() {
  Fixture fx;
  fx.fs.sleepFiles = makeBmps(250);
  auto d = fx.deps();
  // Mark last 10 as favorites; they must all survive.
  d.isFavorite = [](const std::string& path) { return path >= "/sleep/f0240.bmp"; };
  auto& wp = WallpaperPlaylist::instance();
  wp.setDeps(d);
  wp.markFolderDirty();
  wp.reconcile();

  const TrimResult& last = {};  // unused — read back state instead
  (void)last;

  // After trim: count should be kPlaylistMaxPersist. Favorites (last 10) kept.
  TEST_ASSERT_EQUAL(kPlaylistMaxPersist, fx.fs.countSleepBmps(1000));
  for (int i = 240; i < 250; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "f%04d.bmp", i);
    const auto it = std::find(fx.fs.sleepFiles.begin(), fx.fs.sleepFiles.end(), std::string(buf));
    TEST_ASSERT_TRUE_MESSAGE(it != fx.fs.sleepFiles.end(), buf);
  }
  TEST_ASSERT_EQUAL(50, fx.fs.renames.size());  // 250 - 200 moved
}

void test_dirty_reconcile_clears_flag() {
  Fixture fx;
  fx.fs.sleepFiles = makeBmps(10);
  auto& wp = WallpaperPlaylist::instance();
  wp.setDeps(fx.deps());

  wp.markFolderDirty();
  TEST_ASSERT_TRUE(wp.dirty());
  wp.reconcile();
  TEST_ASSERT_FALSE(wp.dirty());
}

void test_remember_rendered_updates_path_and_filename() {
  Fixture fx;
  auto& wp = WallpaperPlaylist::instance();
  wp.setDeps(fx.deps());

  wp.rememberRendered("/sleep/foo.bmp", "foo.bmp");
  TEST_ASSERT_EQUAL_STRING("/sleep/foo.bmp", fx.lastRenderedPath.c_str());
  TEST_ASSERT_EQUAL_STRING("foo.bmp", fx.lastShownFilename.c_str());
  TEST_ASSERT_EQUAL(1, fx.saves);

  // Idempotent: second call with same values does not persist.
  wp.rememberRendered("/sleep/foo.bmp", "foo.bmp");
  TEST_ASSERT_EQUAL(1, fx.saves);
}

void test_small_advance_single_scan() {
  Fixture fx;
  fx.fs.sleepFiles = makeBmps(50);
  auto& wp = WallpaperPlaylist::instance();
  wp.setDeps(fx.deps());

  fx.fs.scanCount = 0;
  wp.advance();
  TEST_ASSERT_EQUAL_MESSAGE(1, fx.fs.scanCount, "First Small advance must scan once");

  fx.fs.scanCount = 0;
  wp.advance();
  TEST_ASSERT_EQUAL_MESSAGE(1, fx.fs.scanCount, "Subsequent Small advance must scan once");
}

void test_large_advance_single_scan_pair() {
  Fixture fx;
  fx.fs.sleepFiles = makeBmps(kSmallToLargeThreshold + 5);
  auto& wp = WallpaperPlaylist::instance();
  wp.setDeps(fx.deps());
  wp.advance();  // Small→Large migration; not counted

  fx.fs.scanCount = 0;
  wp.advance();
  // Large steady state: countSleepBmps for hysteresis + nextSleepBmpAfter for advance.
  // Only countSleepBmps/listSleepBmps counted — nextSleepBmpAfter is not a full scan in this counter.
  TEST_ASSERT_EQUAL_MESSAGE(1, fx.fs.scanCount, "Large advance must only count once beyond nextSleepBmpAfter");
}

void test_clear_rendered_path() {
  Fixture fx;
  fx.lastRenderedPath = "/sleep/x.bmp";
  auto& wp = WallpaperPlaylist::instance();
  wp.setDeps(fx.deps());

  wp.clearRenderedPath();
  TEST_ASSERT_TRUE(fx.lastRenderedPath.empty());
  TEST_ASSERT_EQUAL(1, fx.saves);

  // Idempotent on empty.
  wp.clearRenderedPath();
  TEST_ASSERT_EQUAL(1, fx.saves);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_small_empty_folder_advance_returns_empty);
  RUN_TEST(test_small_first_advance_populates_playlist_and_returns_front);
  RUN_TEST(test_small_second_advance_rotates_front_to_back);
  RUN_TEST(test_small_resync_drops_missing_files);
  RUN_TEST(test_small_resync_inserts_new_files_at_cursor);
  RUN_TEST(test_large_advance_uses_next_after);
  RUN_TEST(test_large_advance_wraps_at_end);
  RUN_TEST(test_hysteresis_195_small_stays_small);
  RUN_TEST(test_hysteresis_195_large_stays_large);
  RUN_TEST(test_hysteresis_downgrade_below_190);
  RUN_TEST(test_reshuffle_small_sets_playlist);
  RUN_TEST(test_reshuffle_large_sets_lastShown);
  RUN_TEST(test_trim_moves_overflow_preserves_favorites);
  RUN_TEST(test_dirty_reconcile_clears_flag);
  RUN_TEST(test_remember_rendered_updates_path_and_filename);
  RUN_TEST(test_small_advance_single_scan);
  RUN_TEST(test_large_advance_single_scan_pair);
  RUN_TEST(test_clear_rendered_path);
  return UNITY_END();
}
