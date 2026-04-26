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

  size_t countSleepBmps(size_t /*scanCap*/) override {
    size_t n = 0;
    for (const auto& f : sleepFiles)
      if (isBmp(f.name)) ++n;
    return n;
  }
  std::vector<std::string> listSleepBmps(size_t maxEntries) override {
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
      sleepFiles.erase(std::remove_if(sleepFiles.begin(), sleepFiles.end(),
                                      [&name](const Entry& e) { return e.name == name; }),
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
  std::vector<uint16_t> trimMovedLog;
  int favoritesCapBlockedLog = 0;
  std::function<bool(const std::string&)> isFavoriteFn;
  long fakeRandomSeed = 0;

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
    d.onTrimMoved = [this](uint16_t n) { trimMovedLog.push_back(n); };
    d.onFavoritesCapBlocked = [this]() { ++favoritesCapBlockedLog; };
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

void test_new_files_inserted_at_cursor_in_mtime_order() {
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
  TEST_ASSERT_EQUAL_STRING("zz_new1.bmp", a.c_str());
  TEST_ASSERT_EQUAL_STRING("zz_new2.bmp", b.c_str());
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

  TEST_ASSERT_EQUAL(1u, fx.trimMovedLog.size());
  TEST_ASSERT_EQUAL(2u, fx.trimMovedLog[0]);
  TEST_ASSERT_EQUAL(2u, fx.fs.renames.size());
  TEST_ASSERT_EQUAL_STRING("/sleep/old_0.bmp", fx.fs.renames[0].first.c_str());
  TEST_ASSERT_EQUAL_STRING("/sleep/old_1.bmp", fx.fs.renames[1].first.c_str());
  TEST_ASSERT_EQUAL_STRING("/sleep pause/old_0.bmp", fx.fs.renames[0].second.c_str());
}

void test_favorites_full_blocks_new_uploads() {
  Fixture fx;
  for (int i = 0; i < 500; ++i) fx.fs.seed(std::string("fav_") + std::to_string(i) + ".bmp", 100);
  fx.fs.seed("new_drop.bmp", 999);
  fx.isFavoriteFn = [](const std::string& path) {
    return path.find("fav_") != std::string::npos;
  };

  auto& wp = crosspoint::sleep::v2::WallpaperPlaylistV2::instance();
  wp.resetForTest();
  fx.wire(wp);
  wp.markFolderDirty();
  wp.reconcile();

  TEST_ASSERT_EQUAL(1, fx.favoritesCapBlockedLog);
  bool newDropMoved = false;
  for (const auto& r : fx.fs.renames) {
    if (r.first == "/sleep/new_drop.bmp") newDropMoved = true;
  }
  TEST_ASSERT_TRUE(newDropMoved);
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

  fx.fs.sleepFiles.erase(std::remove_if(fx.fs.sleepFiles.begin(), fx.fs.sleepFiles.end(),
                                        [](const FakeSleepFs::Entry& e) {
                                          return e.name == "f1.bmp" || e.name == "f2.bmp";
                                        }),
                         fx.fs.sleepFiles.end());

  for (int i = 0; i < 6; ++i) {
    const auto n = wp.advance();
    TEST_ASSERT_TRUE(n != "f1.bmp");
    TEST_ASSERT_TRUE(n != "f2.bmp");
  }
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_advance_with_4_files_walks_all_then_reshuffles);
  RUN_TEST(test_advance_persists_cursor_across_simulated_reboot);
  RUN_TEST(test_new_files_inserted_at_cursor_in_mtime_order);
  RUN_TEST(test_trim_pushes_oldest_mtime_non_favorites);
  RUN_TEST(test_favorites_full_blocks_new_uploads);
  RUN_TEST(test_reshuffle_clears_buffer_when_sleep_empty);
  RUN_TEST(test_advance_skips_files_deleted_since_reconcile);
  return UNITY_END();
}
