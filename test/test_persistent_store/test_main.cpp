/**
 * Host-side tests for persist::PersistentStore<T>, InMemoryFileIO, and the
 * CrossPointState JSON pair. No ESP32/SdFat — runs on developer machine via
 *   pio test -e test_host
 */
#include <ArduinoJson.h>
#include <unity.h>

#include <string>

#include "CrossPointState.h"
#include "persist/CrossPointStateJson.h"
#include "persist/InMemoryFileIO.h"
#include "persist/PersistManager.h"
#include "persist/PersistentStore.h"

using crosspoint::persist::InMemoryFileIO;
using crosspoint::persist::LoadReport;
using crosspoint::persist::PersistentStore;

// ===== Minimal test schema for behavior tests =====
struct MiniState {
  int counter = 0;
  std::string label;
  bool flag = false;
};

static std::string miniSerialize(const MiniState& s) {
  JsonDocument doc;
  doc["counter"] = s.counter;
  doc["label"] = s.label;
  doc["flag"] = s.flag;
  std::string out;
  serializeJson(doc, out);
  return out;
}

static bool miniDeserialize(const std::string& json, MiniState& s) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  s.counter = doc["counter"] | 0;
  s.label = doc["label"] | std::string("");
  s.flag = doc["flag"] | false;
  return true;
}

namespace {
const char* kMiniPath = "/.crosspoint/mini.json";
}

void setUp() {}
void tearDown() {}

// ---- Round-trip: set → flush → load → equal ----
void test_round_trip() {
  InMemoryFileIO io;
  PersistentStore<MiniState> store("MINI", kMiniPath, io, &miniSerialize, &miniDeserialize);

  store.set(&MiniState::counter, 42);
  store.set(&MiniState::label, std::string("hello"));
  store.set(&MiniState::flag, true);
  TEST_ASSERT_TRUE(store.flushNow());
  TEST_ASSERT_FALSE(store.isDirty());

  PersistentStore<MiniState> loader("MINI", kMiniPath, io, &miniSerialize, &miniDeserialize);
  auto r = loader.load();
  TEST_ASSERT_EQUAL_INT(LoadReport::kOk, r.status);
  TEST_ASSERT_EQUAL_INT(42, loader.get().counter);
  TEST_ASSERT_EQUAL_STRING("hello", loader.get().label.c_str());
  TEST_ASSERT_TRUE(loader.get().flag);
}

// ---- Debounce coalescing: N rapid sets → exactly 1 write ----
void test_debounce_coalesces() {
  InMemoryFileIO io;
  PersistentStore<MiniState> store("MINI", kMiniPath, io, &miniSerialize, &miniDeserialize);
  store.setDebounce(1000);

  // Seed clock so dirtyAtMs is anchored.
  crosspoint::persist::setStoreClockForTest(store, 1000);
  for (int i = 0; i < 10; ++i) {
    store.set(&MiniState::counter, i);  // all within the same debounce window
  }
  TEST_ASSERT_TRUE(store.isDirty());
  TEST_ASSERT_EQUAL_size_t(0, io.writeCount());

  // Tick before window elapses — no flush.
  TEST_ASSERT_FALSE(store.tickPersist(1500));
  TEST_ASSERT_EQUAL_size_t(0, io.writeCount());

  // Tick after window elapses — single flush with final value.
  TEST_ASSERT_TRUE(store.tickPersist(2500));
  TEST_ASSERT_EQUAL_size_t(1, io.writeCount());
  TEST_ASSERT_FALSE(store.isDirty());

  PersistentStore<MiniState> loader("MINI", kMiniPath, io, &miniSerialize, &miniDeserialize);
  loader.load();
  TEST_ASSERT_EQUAL_INT(9, loader.get().counter);  // only the last value persisted
}

// ---- Defaults on missing file ----
void test_defaults_on_missing() {
  InMemoryFileIO io;  // empty
  PersistentStore<MiniState> store("MINI", kMiniPath, io, &miniSerialize, &miniDeserialize);
  auto r = store.load();
  TEST_ASSERT_EQUAL_INT(LoadReport::kMissing, r.status);
  TEST_ASSERT_EQUAL_INT(0, store.get().counter);
  TEST_ASSERT_EQUAL_STRING("", store.get().label.c_str());
  TEST_ASSERT_FALSE(store.get().flag);
}

// ---- Corrupt JSON → defaults + kCorrupt report ----
void test_corrupt_reports() {
  InMemoryFileIO io;
  io.put(kMiniPath, "{not valid json");
  PersistentStore<MiniState> store("MINI", kMiniPath, io, &miniSerialize, &miniDeserialize);
  auto r = store.load();
  TEST_ASSERT_EQUAL_INT(LoadReport::kCorrupt, r.status);
  TEST_ASSERT_EQUAL_INT(0, store.get().counter);  // defaults retained
}

// ---- Atomic recovery: crash during promote step → next load gets .bak content ----
void test_atomic_recovery_from_bak() {
  InMemoryFileIO io;
  PersistentStore<MiniState> store("MINI", kMiniPath, io, &miniSerialize, &miniDeserialize);

  // First write: real now has {counter:1}
  store.set(&MiniState::counter, 1);
  TEST_ASSERT_TRUE(store.flushNow());
  TEST_ASSERT_TRUE(io.has(kMiniPath));

  // Second write simulates crash at step 4 (promote .tmp → real). Between
  // step 3 and 4, real was rotated to .bak — and step 4 failure leaves
  // real absent but .bak holding the prior good content.
  store.set(&MiniState::counter, 99);
  io.failNextWriteAtStep(4);
  TEST_ASSERT_FALSE(store.flushNow());
  TEST_ASSERT_FALSE(io.has(kMiniPath));                       // real gone
  TEST_ASSERT_TRUE(io.has(std::string(kMiniPath) + ".bak"));  // bak has prior good

  // Fresh store loads — safeRead chain: real missing → bak → content returned.
  PersistentStore<MiniState> loader("MINI", kMiniPath, io, &miniSerialize, &miniDeserialize);
  auto r = loader.load();
  TEST_ASSERT_EQUAL_INT(LoadReport::kOk, r.status);
  TEST_ASSERT_EQUAL_INT(1, loader.get().counter);  // pre-crash value recovered
}

// ---- flushSoon vs flushNow — mutate coalesces with set ----
void test_mutate_and_flushnow() {
  InMemoryFileIO io;
  PersistentStore<MiniState> store("MINI", kMiniPath, io, &miniSerialize, &miniDeserialize);

  store.mutate([](MiniState& s) {
    s.counter = 7;
    s.label = "x";
  });
  TEST_ASSERT_TRUE(store.isDirty());
  TEST_ASSERT_TRUE(store.flushNow());
  TEST_ASSERT_EQUAL_size_t(1, io.writeCount());
  TEST_ASSERT_FALSE(store.isDirty());

  // No pending mutations — flushNow is a no-op on clean store (still rewrites
  // but shouldn't mark anything wrong).
  TEST_ASSERT_TRUE(store.flushNow());
  TEST_ASSERT_EQUAL_size_t(2, io.writeCount());
}

// ---- CrossPointState round-trip via the real JSON pair ----
void test_cross_point_state_round_trip() {
  CrossPointState original;
  original.openEpubPath = "/books/alice.epub";
  original.lastSleepImage = 7;
  original.lastShownSleepFilename = "wall_042.bmp";
  original.readerActivityLoadCount = 3;
  original.sessionPagesRead = 128;
  original.lastSleepFromReader = true;
  original.wallpaperRotationPaused = true;
  original.lastSleepWasQuotes = false;
  original.sleepImagePlaylist = {"a.bmp", "b.bmp", "c.bmp"};
  original.favoriteBmpPaths = {"/sleep/fav1.bmp"};

  const std::string json = crosspoint::persist::serializeCrossPointState(original);
  TEST_ASSERT_TRUE(json.size() > 0);

  CrossPointState loaded;
  TEST_ASSERT_TRUE(crosspoint::persist::deserializeCrossPointState(json, loaded));

  TEST_ASSERT_EQUAL_STRING(original.openEpubPath.c_str(), loaded.openEpubPath.c_str());
  TEST_ASSERT_EQUAL_UINT8(original.lastSleepImage, loaded.lastSleepImage);
  TEST_ASSERT_EQUAL_STRING(original.lastShownSleepFilename.c_str(), loaded.lastShownSleepFilename.c_str());
  TEST_ASSERT_EQUAL_UINT8(original.readerActivityLoadCount, loaded.readerActivityLoadCount);
  TEST_ASSERT_EQUAL_UINT32(original.sessionPagesRead, loaded.sessionPagesRead);
  TEST_ASSERT_TRUE(loaded.lastSleepFromReader);
  TEST_ASSERT_TRUE(loaded.wallpaperRotationPaused);
  TEST_ASSERT_FALSE(loaded.lastSleepWasQuotes);
  TEST_ASSERT_EQUAL_size_t(3, loaded.sleepImagePlaylist.size());
  TEST_ASSERT_EQUAL_STRING("b.bmp", loaded.sleepImagePlaylist[1].c_str());
  TEST_ASSERT_EQUAL_size_t(1, loaded.favoriteBmpPaths.size());
}

// ---- Large playlist is omitted on write (matches V1 behavior) ----
void test_large_playlist_omitted() {
  CrossPointState s;
  for (size_t i = 0; i < CrossPointState::SLEEP_PLAYLIST_MAX_PERSIST + 5; ++i) {
    s.sleepImagePlaylist.push_back("w_" + std::to_string(i));
  }
  s.lastShownSleepFilename = "current.bmp";
  const std::string json = crosspoint::persist::serializeCrossPointState(s);

  // Playlist key should be absent in the serialized JSON.
  TEST_ASSERT_EQUAL_INT(std::string::npos, (int)json.find("\"sleepImagePlaylist\""));
  TEST_ASSERT_TRUE(json.find("\"lastShownSleepFilename\":\"current.bmp\"") != std::string::npos);
}

// ---- PersistManager flushAll ticks only dirty stores ----
void test_persist_manager_flush_all() {
  using crosspoint::persist::PersistManagerImpl;
  PersistManagerImpl mgr;
  InMemoryFileIO io;
  PersistentStore<MiniState> s1("S1", "/.crosspoint/s1.json", io, &miniSerialize, &miniDeserialize);
  PersistentStore<MiniState> s2("S2", "/.crosspoint/s2.json", io, &miniSerialize, &miniDeserialize);

  auto reg = [&mgr](PersistentStore<MiniState>& s) {
    mgr.registerStore(PersistManagerImpl::Entry{
        [&s](uint32_t now) { return s.tickPersist(now); },
        [&s]() { return s.flushNow(); },
        [&s]() { return s.isDirty(); },
        s.name(),
        s.path(),
    });
  };
  reg(s1);
  reg(s2);

  s1.set(&MiniState::counter, 1);
  // s2 stays clean.
  const size_t flushed = mgr.flushAll();
  TEST_ASSERT_EQUAL_size_t(1, flushed);
  TEST_ASSERT_FALSE(s1.isDirty());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_round_trip);
  RUN_TEST(test_debounce_coalesces);
  RUN_TEST(test_defaults_on_missing);
  RUN_TEST(test_corrupt_reports);
  RUN_TEST(test_atomic_recovery_from_bak);
  RUN_TEST(test_mutate_and_flushnow);
  RUN_TEST(test_cross_point_state_round_trip);
  RUN_TEST(test_large_playlist_omitted);
  RUN_TEST(test_persist_manager_flush_all);
  return UNITY_END();
}
