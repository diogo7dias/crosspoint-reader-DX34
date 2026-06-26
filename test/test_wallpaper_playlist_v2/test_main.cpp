// Host-side tests for sleep::v2::WallpaperPlaylistV2.
// Run via: pio test -e test_host -f test_wallpaper_playlist_v2

#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "persist/InMemoryFileIO.h"
#include "sleep/SleepFs.h"
#include "sleep/WallpaperPlaylistV2.h"

using crosspoint::persist::InMemoryFileIO;
using crosspoint::sleep::ISleepFs;

namespace {

class FakeSleepFs : public crosspoint::sleep::ISleepFs {
 public:
  struct Entry {
    std::string name;
    uint32_t mtime = 0;
  };
  std::vector<Entry> sleepFiles;
  std::unordered_set<std::string> existsSet;
  std::vector<std::pair<std::string, std::string>> renames;

  // Counts materializing-listing calls so heap-gate tests can assert the
  // playlist bailed BEFORE building the full name vector.
  size_t listCalls = 0;

  size_t countSleepBmps(size_t /*scanCap*/) override {
    size_t n = 0;
    for (const auto& f : sleepFiles)
      if (isBmp(f.name)) ++n;
    return n;
  }
  void walkSleepBmps(const std::function<void(const char*, size_t, uint32_t)>& cb) override {
    for (const auto& f : sleepFiles)
      if (isBmp(f.name)) cb(f.name.c_str(), f.name.size(), f.mtime);
  }
  std::vector<std::string> listSleepBmps(size_t maxEntries) override {
    ++listCalls;
    std::vector<std::string> out;
    for (const auto& f : sleepFiles)
      if (isBmp(f.name)) out.push_back(f.name);
    if (out.size() > maxEntries) out.resize(maxEntries);
    std::sort(out.begin(), out.end());
    return out;
  }
  std::vector<crosspoint::sleep::SleepBmpEntry> listSleepBmpsWithMtime(size_t maxEntries) override {
    std::vector<crosspoint::sleep::SleepBmpEntry> out;
    for (const auto& f : sleepFiles)
      if (isBmp(f.name)) out.push_back({f.name, f.mtime});
    if (out.size() > maxEntries) out.resize(maxEntries);
    return out;
  }
  std::string nextSleepBmpAfter(const std::string& /*after*/) override { return ""; }
  std::string nthSleepBmp(size_t /*n*/) override { return ""; }
  bool exists(const std::string& path) override {
    if (existsSet.count(path)) return true;
    if (path.compare(0, 7, "/sleep/") == 0) {
      const std::string name = path.substr(7);
      for (const auto& f : sleepFiles)
        if (f.name == name) return true;
    }
    return false;
  }
  bool mkdir(const std::string& /*path*/) override { return true; }
  bool rename(const std::string& from, const std::string& to) override {
    renames.push_back({from, to});
    if (from.compare(0, 7, "/sleep/") == 0) {
      const std::string name = from.substr(7);
      sleepFiles.erase(
          std::remove_if(sleepFiles.begin(), sleepFiles.end(), [&name](const Entry& e) { return e.name == name; }),
          sleepFiles.end());
    }
    return true;
  }

  void seed(const std::string& name, uint32_t mtime) { sleepFiles.push_back({name, mtime}); }

 private:
  static bool isBmp(const std::string& n) {
    return n.size() > 4 && (n.compare(n.size() - 4, 4, ".bmp") == 0 || n.compare(n.size() - 4, 4, ".BMP") == 0);
  }
};

struct Fixture {
  FakeSleepFs fs;
  InMemoryFileIO io;
  std::string lastShownFilename;
  std::string lastRenderedPath;
  std::vector<std::string> savedAppStateLog;
  std::function<bool(const std::string&)> isFavoriteFn;
  std::function<std::string(const std::string&)> favoriteCounterpartFn;
  long fakeRandomSeed = 0;
  // RFC #156 C2: scripted heap probe. Default unlimited. Tests set this to
  // simulate fragmentation and verify the playlist's heap-gated bail paths.
  std::function<size_t()> largestFreeBlockFn;

  void wire(crosspoint::sleep::v2::WallpaperPlaylistV2& wp) {
    crosspoint::sleep::v2::WallpaperPlaylistV2::Deps d;
    d.fs = &fs;
    d.fileIO = &io;
    d.orderFilePath = "/.crosspoint/sleep_order.txt";
    d.lastShownFilename = &lastShownFilename;
    d.lastRenderedPath = &lastRenderedPath;
    d.saveAppState = [this]() {
      savedAppStateLog.push_back(lastShownFilename);
      return true;
    };
    d.randomFn = [this](long mod) -> long {
      const long r = fakeRandomSeed % mod;
      fakeRandomSeed = (fakeRandomSeed * 1103515245 + 12345) & 0x7fffffff;
      return r;
    };
    d.isFavorite = [this](const std::string& path) {
      if (isFavoriteFn) return isFavoriteFn(path);
      return false;
    };
    d.onPathRenamed = [](const std::string&, const std::string&) {};
    d.largestFreeBlockFn = largestFreeBlockFn;  // empty std::function → unlimited
    d.favoriteCounterpartFn = favoriteCounterpartFn;  // empty → rename detection off
    wp.setDeps(d);
  }
};

void test_advance_with_4_files_walks_all_then_reshuffles() {
  Fixture fx;
  for (int i = 0; i < 4; ++i) fx.fs.seed(std::string("f") + char('0' + i) + ".bmp", 100 + i);
  auto& wp = crosspoint::sleep::v2::WallpaperPlaylistV2::instance();
  wp.resetForTest();
  fx.wire(wp);
  wp.markFolderDirty();
  wp.reconcile();

  std::unordered_set<std::string> seen;
  for (int i = 0; i < 4; ++i) {
    auto next = wp.advance();
    TEST_ASSERT_FALSE(next.empty());
    seen.insert(next);
  }
  TEST_ASSERT_EQUAL(4u, seen.size());

  auto fifth = wp.advance();
  TEST_ASSERT_FALSE(fifth.empty());
}

void test_advance_persists_cursor_across_simulated_reboot() {
  Fixture fx;
  for (int i = 0; i < 5; ++i) fx.fs.seed(std::string("a") + char('0' + i) + ".bmp", 100 + i);
  auto& wp = crosspoint::sleep::v2::WallpaperPlaylistV2::instance();
  wp.resetForTest();
  fx.wire(wp);
  wp.markFolderDirty();
  wp.reconcile();

  const auto first = wp.advance();
  const auto second = wp.advance();
  TEST_ASSERT_TRUE(first != second);

  wp.resetForTest();
  fx.wire(wp);

  const auto third = wp.advance();
  TEST_ASSERT_TRUE(first != third);
  TEST_ASSERT_TRUE(second != third);
}

void test_new_files_spliced_at_front_newest_mtime_first() {
  // "New wallpapers on top": fresh uploads land at the FRONT of the buffer,
  // newest mtime first. Cursor resets to 0 so the freshest upload is the very
  // next wallpaper shown.
  Fixture fx;
  for (int i = 0; i < 6; ++i) fx.fs.seed(std::string("base_") + char('a' + i) + ".bmp", 100);
  auto& wp = crosspoint::sleep::v2::WallpaperPlaylistV2::instance();
  wp.resetForTest();
  fx.wire(wp);
  wp.markFolderDirty();
  wp.reconcile();

  wp.advance();
  wp.advance();

  fx.fs.seed("zz_new1.bmp", 500);
  fx.fs.seed("zz_new2.bmp", 600);
  wp.markFolderDirty();
  wp.reconcile();

  const auto a = wp.advance();
  const auto b = wp.advance();
  TEST_ASSERT_EQUAL_STRING("zz_new2.bmp", a.c_str());  // newest mtime → very top
  TEST_ASSERT_EQUAL_STRING("zz_new1.bmp", b.c_str());
}

void test_trim_pushes_oldest_mtime_non_favorites() {
  Fixture fx;
  for (int i = 0; i < 5; ++i) fx.fs.seed(std::string("old_") + std::to_string(i) + ".bmp", 1 + i);
  for (int i = 0; i < 497; ++i) fx.fs.seed(std::string("new_") + std::to_string(i) + ".bmp", 1000 + i);
  fx.isFavoriteFn = [](const std::string&) { return false; };

  auto& wp = crosspoint::sleep::v2::WallpaperPlaylistV2::instance();
  wp.resetForTest();
  fx.wire(wp);
  wp.markFolderDirty();
  wp.reconcile();

  const auto notice = wp.takeNotice();
  TEST_ASSERT_TRUE(notice.reconciled);
  TEST_ASSERT_FALSE(notice.favoritesCapBlocked);
  TEST_ASSERT_EQUAL(2u, notice.movedToPause);
  TEST_ASSERT_EQUAL(2u, fx.fs.renames.size());
  TEST_ASSERT_EQUAL_STRING("/sleep/old_0.bmp", fx.fs.renames[0].first.c_str());
  TEST_ASSERT_EQUAL_STRING("/sleep/old_1.bmp", fx.fs.renames[1].first.c_str());
  TEST_ASSERT_EQUAL_STRING("/sleep pause/old_0.bmp", fx.fs.renames[0].second.c_str());
}

// Bug A (RFC #156): a fragmented heap must make trimToCap bail rather than
// allocate its 3 transient partition vectors and risk a bad_alloc abort. With
// a tiny largestFreeBlock, an over-cap reconcile renames nothing this cycle.
void test_trim_bails_on_fragmented_heap() {
  Fixture fx;
  for (int i = 0; i < 502; ++i) fx.fs.seed(std::string("f_") + std::to_string(i) + ".bmp", 100 + i);
  fx.isFavoriteFn = [](const std::string&) { return false; };
  fx.largestFreeBlockFn = []() -> size_t { return 1024; };  // simulate severe fragmentation

  auto& wp = crosspoint::sleep::v2::WallpaperPlaylistV2::instance();
  wp.resetForTest();
  fx.wire(wp);
  wp.markFolderDirty();
  wp.reconcile();

  const auto notice = wp.takeNotice();
  TEST_ASSERT_EQUAL(0u, notice.movedToPause);
  TEST_ASSERT_EQUAL(0u, fx.fs.renames.size());  // trim bailed, nothing demoted
}

void test_favorites_full_blocks_new_uploads() {
  Fixture fx;
  for (int i = 0; i < 500; ++i) fx.fs.seed(std::string("fav_") + std::to_string(i) + ".bmp", 100);
  fx.fs.seed("new_drop.bmp", 999);
  fx.isFavoriteFn = [](const std::string& path) { return path.find("fav_") != std::string::npos; };

  auto& wp = crosspoint::sleep::v2::WallpaperPlaylistV2::instance();
  wp.resetForTest();
  fx.wire(wp);
  wp.markFolderDirty();
  wp.reconcile();

  const auto notice = wp.takeNotice();
  TEST_ASSERT_TRUE(notice.reconciled);
  TEST_ASSERT_TRUE(notice.favoritesCapBlocked);
  bool newDropMoved = false;
  for (const auto& r : fx.fs.renames) {
    if (r.first == "/sleep/new_drop.bmp") newDropMoved = true;
  }
  TEST_ASSERT_TRUE(newDropMoved);
}

// RFC #145: a reconcile under the cap reports reconciled=true with no notice
// flags, so a previously-persisted favorites-cap warning clears. takeNotice()
// is single-shot — a second drain returns a zeroed Notice.
void test_notice_under_cap_is_clean_and_drains_once() {
  Fixture fx;
  for (int i = 0; i < 4; ++i) fx.fs.seed(std::string("f") + std::to_string(i) + ".bmp", 100 + i);
  fx.isFavoriteFn = [](const std::string&) { return false; };

  auto& wp = crosspoint::sleep::v2::WallpaperPlaylistV2::instance();
  wp.resetForTest();
  fx.wire(wp);
  wp.markFolderDirty();
  wp.reconcile();

  const auto first = wp.takeNotice();
  TEST_ASSERT_TRUE(first.reconciled);
  TEST_ASSERT_FALSE(first.favoritesCapBlocked);
  TEST_ASSERT_EQUAL(0u, first.movedToPause);

  const auto second = wp.takeNotice();
  TEST_ASSERT_FALSE(second.reconciled);
  TEST_ASSERT_FALSE(second.favoritesCapBlocked);
  TEST_ASSERT_EQUAL(0u, second.movedToPause);
}

void test_reshuffle_clears_buffer_when_sleep_empty() {
  Fixture fx;
  auto& wp = crosspoint::sleep::v2::WallpaperPlaylistV2::instance();
  wp.resetForTest();
  fx.wire(wp);
  TEST_ASSERT_FALSE(wp.reshuffle());
  TEST_ASSERT_EQUAL(0u, wp.entryCountForTest());
}

void test_advance_skips_files_deleted_since_reconcile() {
  Fixture fx;
  for (int i = 0; i < 5; ++i) fx.fs.seed(std::string("f") + char('0' + i) + ".bmp", 100);
  auto& wp = crosspoint::sleep::v2::WallpaperPlaylistV2::instance();
  wp.resetForTest();
  fx.wire(wp);
  wp.markFolderDirty();
  wp.reconcile();

  fx.fs.sleepFiles.erase(
      std::remove_if(fx.fs.sleepFiles.begin(), fx.fs.sleepFiles.end(),
                     [](const FakeSleepFs::Entry& e) { return e.name == "f1.bmp" || e.name == "f2.bmp"; }),
      fx.fs.sleepFiles.end());

  for (int i = 0; i < 6; ++i) {
    const auto n = wp.advance();
    TEST_ASSERT_TRUE(n != "f1.bmp");
    TEST_ASSERT_TRUE(n != "f2.bmp");
  }
}

void test_reshuffle_does_not_repeat_just_shown_wallpaper() {
  // 4 files. With seed 0 the FY shuffle ends "f3, f0, f1, f2" — index 0 is
  // f3, which would also be the last file shown by lap 1 under that same
  // PRNG sequence. Pre-fix: f3 shows again immediately after reshuffle.
  Fixture fx;
  for (int i = 0; i < 4; ++i) fx.fs.seed(std::string("f") + char('0' + i) + ".bmp", 100 + i);
  auto& wp = crosspoint::sleep::v2::WallpaperPlaylistV2::instance();
  wp.resetForTest();
  fx.wire(wp);
  wp.markFolderDirty();
  wp.reconcile();

  std::string lastInLap;
  for (int i = 0; i < 4; ++i) lastInLap = wp.advance();
  TEST_ASSERT_FALSE(lastInLap.empty());

  // Drive several reshuffle cycles under different PRNG seeds. The first
  // wallpaper of each new lap must never equal the last of the previous lap.
  for (int trial = 0; trial < 16; ++trial) {
    fx.fakeRandomSeed = static_cast<long>(trial * 31 + 7);
    const auto firstAfterReshuffle = wp.advance();
    TEST_ASSERT_FALSE(firstAfterReshuffle.empty());
    TEST_ASSERT_TRUE_MESSAGE(firstAfterReshuffle != lastInLap, "Just-shown wallpaper appeared again after reshuffle");
    lastInLap = firstAfterReshuffle;
    // Walk to end of this lap to set up the next reshuffle.
    for (int i = 0; i < 2; ++i) {
      const auto n = wp.advance();
      if (!n.empty()) lastInLap = n;
    }
  }
}

// RFC #156 C2: heap-probe injection lets host tests exercise the
// fragmentation-cliff branch that the device-only #ifdef previously
// short-circuited. The probe is consulted before writeBuffer() reserves
// the contiguous buffer; if the reported largest block is below
// (needBytes + 4 KB headroom), the playlist bails with an empty buffer
// and advance() returns "" so the caller (e.g. SleepActivity) can
// fall back to streaming direct pick.

void test_fragmented_heap_blocks_initial_reshuffle_buffer_stays_empty() {
  Fixture fx;
  fx.fs.seed("a.bmp", 100);
  fx.fs.seed("b.bmp", 101);
  fx.fs.seed("c.bmp", 102);
  // Probe reports 1 KB largest free block — far below needBytes + 4 KB
  // headroom for any non-zero buffer. Reshuffle's writeBuffer must bail.
  fx.largestFreeBlockFn = []() -> size_t { return 1024; };
  auto& wp = crosspoint::sleep::v2::WallpaperPlaylistV2::instance();
  wp.resetForTest();
  fx.wire(wp);

  const auto first = wp.advance();
  TEST_ASSERT_TRUE(first.empty());  // bailed — no buffer to walk
  TEST_ASSERT_TRUE(wp.bufferForTest().empty());
}

void test_healthy_heap_allows_reshuffle_and_advance_returns_name() {
  Fixture fx;
  fx.fs.seed("a.bmp", 100);
  fx.fs.seed("b.bmp", 101);
  fx.fs.seed("c.bmp", 102);
  // Probe reports plenty of headroom — playlist behaves as before C2.
  fx.largestFreeBlockFn = []() -> size_t { return 256 * 1024; };
  auto& wp = crosspoint::sleep::v2::WallpaperPlaylistV2::instance();
  wp.resetForTest();
  fx.wire(wp);

  const auto first = wp.advance();
  TEST_ASSERT_FALSE(first.empty());  // reshuffle succeeded
  TEST_ASSERT_FALSE(wp.bufferForTest().empty());
}

// The zero-allocation membership scan matches whole '\n'-delimited names, so a
// name that is a prefix (or superstring) of one already in the buffer must not
// false-match. Here "ab.bmp" is buffered first; adding "a.bmp" (a prefix) must
// be detected as NEW and spliced on top, while "ab.bmp" must NOT be re-added.
void test_membership_scan_handles_prefix_collisions() {
  Fixture fx;
  fx.fs.seed("ab.bmp", 100);
  auto& wp = crosspoint::sleep::v2::WallpaperPlaylistV2::instance();
  wp.resetForTest();
  fx.wire(wp);
  wp.markFolderDirty();
  wp.reconcile();  // buffer = "ab.bmp\n"

  fx.fs.seed("a.bmp", 200);  // prefix of ab.bmp, newer mtime
  wp.markFolderDirty();
  wp.reconcile();

  // a.bmp (newest) spliced to the front → shown next; not dropped as a false
  // prefix match.
  const auto first = wp.advance();
  TEST_ASSERT_EQUAL_STRING("a.bmp", first.c_str());

  // A full lap shows exactly the two distinct files — ab.bmp was not duplicated
  // by a missed membership hit, and neither was dropped.
  std::unordered_set<std::string> seen;
  seen.insert(first);
  seen.insert(wp.advance());
  TEST_ASSERT_EQUAL(2u, seen.size());
  TEST_ASSERT_TRUE(seen.count("a.bmp") == 1 && seen.count("ab.bmp") == 1);
}

void test_unset_heap_probe_treats_heap_as_unlimited() {
  // Mirrors pre-C2 host behaviour: with no probe wired, the playlist must
  // not gate any reserve — every existing test in this file relies on this
  // implicit default. Verify it explicitly so a future refactor that
  // changes the default policy fails loudly here instead of silently
  // breaking the rest of the suite.
  Fixture fx;
  fx.fs.seed("a.bmp", 100);
  // fx.largestFreeBlockFn deliberately left empty.
  auto& wp = crosspoint::sleep::v2::WallpaperPlaylistV2::instance();
  wp.resetForTest();
  fx.wire(wp);

  const auto first = wp.advance();
  TEST_ASSERT_FALSE(first.empty());
}

// A reconcile that truncates its new-file batch (kMaxNewFilesPerReconcile=64)
// must STAY dirty so the next sleep's reconcile absorbs the remainder. Pre-fix
// it cleared dirty_, stranding the excess files until the next upload/reboot.
void test_truncated_reconcile_stays_dirty_and_converges() {
  Fixture fx;
  for (int i = 0; i < 10; ++i) fx.fs.seed("base_" + std::to_string(i) + ".bmp", 100 + i);
  auto& wp = crosspoint::sleep::v2::WallpaperPlaylistV2::instance();
  wp.resetForTest();
  fx.wire(wp);
  wp.markFolderDirty();
  wp.reconcile();
  TEST_ASSERT_EQUAL(10u, wp.entryCountForTest());

  for (int i = 0; i < 100; ++i) fx.fs.seed("new_" + std::to_string(i) + ".bmp", 1000 + i);
  wp.markFolderDirty();
  wp.reconcile();
  TEST_ASSERT_EQUAL(74u, wp.entryCountForTest());  // 10 + one 64-file batch
  TEST_ASSERT_TRUE_MESSAGE(wp.dirty(), "truncated reconcile must keep dirty_ set");

  wp.reconcile();  // NO markFolderDirty — must self-continue
  TEST_ASSERT_EQUAL(110u, wp.entryCountForTest());
  TEST_ASSERT_FALSE(wp.dirty());
}

// User-initiated reshuffle materializes a vector of up to 500 heap strings.
// Under a fragmented heap it must bail BEFORE building that listing (the walk
// is zero-heap; the listing is not) and report failure to the caller.
void test_reshuffle_bails_before_listing_on_fragmented_heap() {
  Fixture fx;
  for (int i = 0; i < 40; ++i)
    fx.fs.seed("very_long_wallpaper_filename_to_inflate_measured_cost_" + std::to_string(i) + ".bmp", 100 + i);
  auto& wp = crosspoint::sleep::v2::WallpaperPlaylistV2::instance();
  wp.resetForTest();
  fx.wire(wp);
  wp.markFolderDirty();
  wp.reconcile();
  const auto bufferBefore = wp.bufferForTest();
  fx.fs.listCalls = 0;

  fx.largestFreeBlockFn = []() -> size_t { return 1024; };
  fx.wire(wp);  // rewire with fragmented probe; setDeps marks dirty, irrelevant here

  TEST_ASSERT_FALSE_MESSAGE(wp.reshuffle(), "reshuffle must report failure under fragmented heap");
  TEST_ASSERT_EQUAL_MESSAGE(0u, fx.fs.listCalls, "reshuffle must not materialize the listing when gated");
}

// Trim-branch re-derive (order file stale + /sleep over cap) used to rebuild
// newFiles UNCAPPED — up to ~500 strings spliced in one pass on the low
// sleep-entry heap. It must respect the same 64-per-pass cap and converge
// across subsequent reconciles.
void test_trim_branch_rederive_capped_per_pass() {
  Fixture fx;
  fx.isFavoriteFn = [](const std::string&) { return false; };
  fx.fs.seed("seed.bmp", 1);
  auto& wp = crosspoint::sleep::v2::WallpaperPlaylistV2::instance();
  wp.resetForTest();
  fx.wire(wp);
  wp.markFolderDirty();
  wp.reconcile();
  TEST_ASSERT_EQUAL(1u, wp.entryCountForTest());

  for (int i = 0; i < 501; ++i) fx.fs.seed("bulk_" + std::to_string(i) + ".bmp", 1000 + i);
  wp.markFolderDirty();
  wp.reconcile();  // disk=502 > cap → trim runs, re-derive must cap at 64
  TEST_ASSERT_TRUE_MESSAGE(wp.entryCountForTest() <= 65u, "re-derived splice must be capped per pass");
  TEST_ASSERT_TRUE_MESSAGE(wp.dirty(), "capped trim-branch reconcile must stay dirty");

  for (int pass = 0; pass < 12 && wp.dirty(); ++pass) wp.reconcile();
  TEST_ASSERT_FALSE(wp.dirty());
  // seed.bmp (demoted by trim) stays as a stale buffer line; all 500 survivors present.
  TEST_ASSERT_EQUAL(501u, wp.entryCountForTest());
}

// Toggles the favorite "_F" suffix before the 4-char extension, mirroring
// FavoriteImage::add/stripFavoriteSuffix. "x.bmp" <-> "x_F.bmp".
std::string toggleFavoriteSuffix(const std::string& n) {
  if (n.size() < 5) return n;
  const std::string ext = n.substr(n.size() - 4);
  const std::string stem = n.substr(0, n.size() - 4);
  if (stem.size() >= 2 && stem.compare(stem.size() - 2, 2, "_F") == 0) {
    return stem.substr(0, stem.size() - 2) + ext;
  }
  return stem + "_F" + ext;
}

// Favoriting a /sleep image renames it (img_b.bmp -> img_b_F.bmp). Pre-fix,
// reconcile saw the renamed file as a brand-new upload and spliced it to the
// FRONT (cursor=0), re-showing the just-favorited image on the very next lock
// instead of advancing to the next wallpaper in the rotation array. With
// favoriteCounterpartFn wired, reconcile recognizes the rename and replaces the
// name in place, so the image keeps its rotation slot and the next lock
// advances normally.
void test_favorite_rename_keeps_rotation_position_not_front() {
  Fixture fx;
  // Identical mtime so the sequential build order is the stable insertion order
  // img_a..img_f, which also matches lexicographic order.
  for (int i = 0; i < 6; ++i) fx.fs.seed(std::string("img_") + char('a' + i) + ".bmp", 100);
  fx.favoriteCounterpartFn = &toggleFavoriteSuffix;

  auto& wp = crosspoint::sleep::v2::WallpaperPlaylistV2::instance();
  wp.resetForTest();
  fx.wire(wp);
  wp.markFolderDirty();
  wp.reconcile();

  const auto first = wp.advance();   // img_a.bmp
  const auto second = wp.advance();  // img_b.bmp — the one the user is about to favorite
  TEST_ASSERT_EQUAL_STRING("img_a.bmp", first.c_str());
  TEST_ASSERT_EQUAL_STRING("img_b.bmp", second.c_str());

  // Device-side favorite: rename on disk; lastShownSleepFilename would also be
  // updated by FavoriteImage, but the rotation buffer still holds the old name.
  const std::string favName = toggleFavoriteSuffix(second);  // img_b_F.bmp
  fx.fs.rename("/sleep/" + second, "/sleep/" + favName);
  fx.fs.seed(favName, 100);

  // Wake-to-home marks the folder dirty; the next lock reconciles.
  wp.markFolderDirty();
  wp.reconcile();

  const auto third = wp.advance();
  TEST_ASSERT_TRUE_MESSAGE(third != favName, "favorited image must not jump to the front and re-show");
  TEST_ASSERT_TRUE_MESSAGE(third != second, "rotation must advance past the favorited image");
  TEST_ASSERT_EQUAL_STRING("img_c.bmp", third.c_str());

  // The favorite is still in rotation (replaced in place, not dropped): a full
  // lap from here reaches img_b_F.bmp exactly once and never the stale
  // img_b.bmp. Collect a fresh post-favorite lap (6 advances covers all files).
  std::unordered_set<std::string> seen;
  seen.insert(third);
  for (int i = 0; i < 6; ++i) seen.insert(wp.advance());
  TEST_ASSERT_EQUAL_MESSAGE(6u, seen.size(), "lap must show all six current files exactly once");
  TEST_ASSERT_TRUE_MESSAGE(seen.count(favName) == 1, "favorited image must remain in rotation under its new name");
  TEST_ASSERT_TRUE_MESSAGE(seen.count("img_b.bmp") == 0, "stale pre-favorite name must not be shown");
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_advance_with_4_files_walks_all_then_reshuffles);
  RUN_TEST(test_advance_persists_cursor_across_simulated_reboot);
  RUN_TEST(test_new_files_spliced_at_front_newest_mtime_first);
  RUN_TEST(test_trim_pushes_oldest_mtime_non_favorites);
  RUN_TEST(test_trim_bails_on_fragmented_heap);
  RUN_TEST(test_favorites_full_blocks_new_uploads);
  RUN_TEST(test_notice_under_cap_is_clean_and_drains_once);
  RUN_TEST(test_reshuffle_clears_buffer_when_sleep_empty);
  RUN_TEST(test_advance_skips_files_deleted_since_reconcile);
  RUN_TEST(test_reshuffle_does_not_repeat_just_shown_wallpaper);
  RUN_TEST(test_fragmented_heap_blocks_initial_reshuffle_buffer_stays_empty);
  RUN_TEST(test_healthy_heap_allows_reshuffle_and_advance_returns_name);
  RUN_TEST(test_membership_scan_handles_prefix_collisions);
  RUN_TEST(test_unset_heap_probe_treats_heap_as_unlimited);
  RUN_TEST(test_truncated_reconcile_stays_dirty_and_converges);
  RUN_TEST(test_reshuffle_bails_before_listing_on_fragmented_heap);
  RUN_TEST(test_trim_branch_rederive_capped_per_pass);
  RUN_TEST(test_favorite_rename_keeps_rotation_position_not_front);
  return UNITY_END();
}
