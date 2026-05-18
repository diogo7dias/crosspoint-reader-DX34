// Memory-stability harness tests.
//
// Exercises production code that touches `deps_.largestFreeBlockFn` (the
// pre-allocation heap probe) under deterministic fragmentation scenarios.
// The goal is to guarantee that no allocation path inside the playlist
// reaches `std::string::reserve` (or similar bad_alloc-throwing call)
// when the heap can't satisfy it — production builds compile with
// `-fno-exceptions`, so a single bad_alloc aborts the firmware.
//
// Run via: pio test -e test_host -f test_memory_harness
#include <unity.h>

#include <cstdint>
#include <string>
#include <vector>

#include "FakeHeap.h"
#include "persist/InMemoryFileIO.h"
#include "sleep/SleepFs.h"
#include "sleep/WallpaperPlaylistV2.h"

using crosspoint::persist::InMemoryFileIO;
using crosspoint::sleep::ISleepFs;
using crosspoint::sleep::SleepBmpEntry;
using crosspoint::sleep::v2::WallpaperPlaylistV2;
using crosspoint::test::FakeHeap;
using crosspoint::test::kCrash1Preconditions;
using crosspoint::test::kCrash2Preconditions;
using crosspoint::test::kHealthyBoot;
using crosspoint::test::applyScenario;

namespace {

class FakeSleepFs : public ISleepFs {
 public:
  std::vector<std::string> files;

  size_t countSleepBmps(size_t) override { return files.size(); }
  std::vector<std::string> listSleepBmps(size_t maxEntries) override {
    std::vector<std::string> out = files;
    if (out.size() > maxEntries) out.resize(maxEntries);
    return out;
  }
  std::vector<SleepBmpEntry> listSleepBmpsWithMtime(size_t maxEntries) override {
    std::vector<SleepBmpEntry> out;
    uint32_t t = 1000;
    for (const auto& f : files) {
      out.push_back({f, t++});
      if (out.size() >= maxEntries) break;
    }
    return out;
  }
  std::string nextSleepBmpAfter(const std::string& after) override {
    std::string best;
    for (const auto& f : files) {
      if (f > after && (best.empty() || f < best)) best = f;
    }
    if (best.empty() && !files.empty()) best = files.front();
    return best;
  }
  std::string nthSleepBmp(size_t n) override {
    return n < files.size() ? files[n] : std::string();
  }
  bool exists(const std::string& path) override {
    // Path comes in as "/sleep/<basename>" — match the basename against `files`.
    const auto slash = path.find_last_of('/');
    const std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    for (const auto& f : files) if (f == base) return true;
    return false;
  }
  bool mkdir(const std::string&) override { return true; }
  bool rename(const std::string&, const std::string&) override { return true; }
};

WallpaperPlaylistV2::Deps makeDeps(FakeSleepFs* fs, InMemoryFileIO* io,
                                   std::string* lastShown, std::string* lastRendered) {
  WallpaperPlaylistV2::Deps deps;
  deps.fs = fs;
  deps.fileIO = io;
  deps.orderFilePath = "/.crosspoint/sleep_order.txt";
  deps.lastShownFilename = lastShown;
  deps.lastRenderedPath = lastRendered;
  deps.saveAppState = []() { return true; };
  deps.randomFn = [](long n) { return n > 0 ? 0L : 0L; };
  deps.isFavorite = [](const std::string&) { return false; };
  deps.onPathRenamed = [](const std::string&, const std::string&) {};
  deps.onTrimMoved = [](uint16_t) {};
  deps.onFavoritesCapBlocked = []() {};
  deps.largestFreeBlockFn = []() { return FakeHeap::largestFreeBlock(); };
  return deps;
}

// ---- Tests --------------------------------------------------------------

void test_healthy_heap_permits_full_reconcile() {
  applyScenario(kHealthyBoot);
  FakeSleepFs fs;
  for (int i = 0; i < 50; ++i) fs.files.push_back("img_" + std::to_string(i) + ".bmp");
  InMemoryFileIO io;
  std::string lastShown, lastRendered;

  auto& playlist = WallpaperPlaylistV2::instance();
  playlist.resetForTest();
  playlist.setDeps(makeDeps(&fs, &io, &lastShown, &lastRendered));
  playlist.markFolderDirty();
  playlist.reconcile();

  TEST_ASSERT_EQUAL_size_t(50, playlist.entryCountForTest());
  TEST_ASSERT_EQUAL(0, FakeHeap::allocFailures());
}

void test_fragmented_heap_skips_buffer_grow_no_crash() {
  // Crash 1 conditions: ~25 KB largest block. The playlist reserves the
  // full buffer (one std::string of ~10 KB at the 500 cap; at 50 entries
  // the buffer is ~600 bytes). Probe headroom is ~4 KB. So 25 KB easily
  // fits 50 entries even with headroom — the test is that the probe is
  // consulted and the path does not bypass it.
  applyScenario(kCrash1Preconditions);
  FakeSleepFs fs;
  for (int i = 0; i < 50; ++i) fs.files.push_back("img_" + std::to_string(i) + ".bmp");
  InMemoryFileIO io;
  std::string lastShown, lastRendered;

  auto& playlist = WallpaperPlaylistV2::instance();
  playlist.resetForTest();
  playlist.setDeps(makeDeps(&fs, &io, &lastShown, &lastRendered));
  playlist.markFolderDirty();
  playlist.reconcile();

  TEST_ASSERT_GREATER_THAN(0, FakeHeap::probeCount());
  TEST_ASSERT_EQUAL(0, FakeHeap::allocFailures());
  // Buffer should still be populated — 25 KB is enough headroom for 50 entries.
  TEST_ASSERT_EQUAL_size_t(50, playlist.entryCountForTest());
}

void test_severely_fragmented_heap_aborts_grow_gracefully() {
  // Largest contiguous block below the probe headroom (4 KB) + needed size.
  // For a brand-new buffer at 500 cap (~10 KB) this should refuse to grow.
  FakeHeap::reset();
  FakeHeap::setTotalFree(80000);
  FakeHeap::setLargestFreeBlock(2000);  // tiny — must refuse all but trivial allocs
  FakeHeap::setMinFreeEver(2000);

  FakeSleepFs fs;
  for (int i = 0; i < 500; ++i) fs.files.push_back("img_" + std::to_string(i) + ".bmp");
  InMemoryFileIO io;
  std::string lastShown, lastRendered;

  auto& playlist = WallpaperPlaylistV2::instance();
  playlist.resetForTest();
  playlist.setDeps(makeDeps(&fs, &io, &lastShown, &lastRendered));
  playlist.markFolderDirty();
  // Must not crash. Buffer may be empty (probe refuses) or partially built.
  playlist.reconcile();
  // The test passes if we get here without aborting.
  TEST_ASSERT_GREATER_THAN(0, FakeHeap::probeCount());
}

void test_advance_under_fragmentation_returns_or_empty() {
  // Buffer-empty + fragmented heap path. advance() should either return a
  // name (if probe permits rebuild) or return empty (refuse cleanly).
  // Either way: no abort.
  applyScenario(kCrash2Preconditions);
  FakeSleepFs fs;
  for (int i = 0; i < 10; ++i) fs.files.push_back("img_" + std::to_string(i) + ".bmp");
  InMemoryFileIO io;
  std::string lastShown, lastRendered;

  auto& playlist = WallpaperPlaylistV2::instance();
  playlist.resetForTest();
  playlist.setDeps(makeDeps(&fs, &io, &lastShown, &lastRendered));
  playlist.markFolderDirty();
  playlist.reconcile();
  std::string first = playlist.advance();
  // 42 KB largest block easily fits 10 entries → must succeed.
  TEST_ASSERT_FALSE(first.empty());
}

void test_heap_crumbles_mid_session_no_abort() {
  // Start healthy, run a reconcile, then fragment heap and call advance.
  // Models the on-device pattern where the section-load path consumes
  // heap fragments and a later wallpaper rotation must cope.
  applyScenario(kHealthyBoot);
  FakeSleepFs fs;
  for (int i = 0; i < 100; ++i) fs.files.push_back("img_" + std::to_string(i) + ".bmp");
  InMemoryFileIO io;
  std::string lastShown, lastRendered;

  auto& playlist = WallpaperPlaylistV2::instance();
  playlist.resetForTest();
  playlist.setDeps(makeDeps(&fs, &io, &lastShown, &lastRendered));
  playlist.markFolderDirty();
  playlist.reconcile();
  std::string first = playlist.advance();
  TEST_ASSERT_FALSE(first.empty());

  // Now fragment hard — largest crumbles to 1 KB. Already-loaded buffer is
  // in RAM so advance() must succeed without any new allocation.
  FakeHeap::fragment(1000);
  for (int i = 0; i < 50; ++i) {
    std::string next = playlist.advance();
    TEST_ASSERT_FALSE(next.empty());
  }
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_healthy_heap_permits_full_reconcile);
  RUN_TEST(test_fragmented_heap_skips_buffer_grow_no_crash);
  RUN_TEST(test_severely_fragmented_heap_aborts_grow_gracefully);
  RUN_TEST(test_advance_under_fragmentation_returns_or_empty);
  RUN_TEST(test_heap_crumbles_mid_session_no_abort);
  return UNITY_END();
}
