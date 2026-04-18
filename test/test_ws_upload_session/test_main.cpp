// Host-side tests for crosspoint::ws::WsUploadSession (RFC #24).
//
// Run via:  pio test -e test_host -f test_ws_upload_session
//
// All file and network I/O is faked through the WsUploadDeps lambda injection
// points. No Arduino, no SdFat.

#include <unity.h>

#include <string>
#include <vector>

#include "network/ws/WsUploadSession.h"

using crosspoint::ws::WsUploadDeps;
using crosspoint::ws::WsUploadSession;

namespace {

struct Fake {
  std::vector<std::pair<uint8_t, std::string>> sent;       // (client, text)
  std::vector<std::pair<std::string, std::string>> opened; // (path, name)
  std::vector<size_t> writes;                              // byte counts
  std::vector<std::pair<std::string, std::string>> cacheCleared;
  int closeAndDelete = 0;
  int closeFinalize = 0;

  bool beginWriteOk = true;
  bool writeBytesOk = true;
  uint32_t now = 1000;
};

Fake g_fake;

WsUploadDeps makeDeps() {
  WsUploadDeps d;
  d.beginWrite = [](const std::string& path, const std::string& name) {
    g_fake.opened.emplace_back(path, name);
    return g_fake.beginWriteOk;
  };
  d.writeBytes = [](const uint8_t*, size_t len) {
    g_fake.writes.push_back(len);
    return g_fake.writeBytesOk;
  };
  d.closeAndDelete = [] { g_fake.closeAndDelete++; };
  d.closeFinalize = [] { g_fake.closeFinalize++; };
  d.sendText = [](uint8_t c, const std::string& t) {
    g_fake.sent.emplace_back(c, t);
  };
  d.nowMs = [] { return g_fake.now; };
  d.clearBookCache = [](const std::string& path, const std::string& name) {
    g_fake.cacheCleared.emplace_back(path, name);
  };
  return d;
}

const std::string& lastSent() {
  TEST_ASSERT_FALSE(g_fake.sent.empty());
  return g_fake.sent.back().second;
}

}  // namespace

void setUp() {
  g_fake = Fake{};
}
void tearDown() {}

// ---- START parsing and session opening ----

void test_start_valid_opens_file_and_sends_ready() {
  WsUploadSession s(makeDeps());
  auto r = s.onStart(3, "START:book.epub:1024:/books");
  TEST_ASSERT_EQUAL(static_cast<int>(WsUploadSession::Result::Ok),
                    static_cast<int>(r));
  TEST_ASSERT_TRUE(s.inProgress());
  TEST_ASSERT_EQUAL(3, s.ownerClient());
  TEST_ASSERT_EQUAL_size_t(1, g_fake.opened.size());
  TEST_ASSERT_EQUAL_STRING("/books", g_fake.opened[0].first.c_str());
  TEST_ASSERT_EQUAL_STRING("book.epub", g_fake.opened[0].second.c_str());
  TEST_ASSERT_EQUAL_STRING("READY", lastSent().c_str());
}

void test_start_while_active_rejects_new_client() {
  WsUploadSession s(makeDeps());
  s.onStart(1, "START:a.epub:10:/");
  g_fake.sent.clear();

  auto r = s.onStart(2, "START:b.epub:10:/");
  TEST_ASSERT_EQUAL(static_cast<int>(WsUploadSession::Result::Rejected),
                    static_cast<int>(r));
  TEST_ASSERT_EQUAL(2, g_fake.sent.back().first);  // sent to client 2
  TEST_ASSERT_EQUAL_STRING("ERROR:Upload already in progress",
                           lastSent().c_str());
  TEST_ASSERT_EQUAL(1, s.ownerClient());  // owner unchanged
  TEST_ASSERT_EQUAL_size_t(1, g_fake.opened.size());  // no second open
}

void test_start_bad_format_rejects() {
  WsUploadSession s(makeDeps());
  auto r = s.onStart(1, "START:malformed");
  TEST_ASSERT_EQUAL(static_cast<int>(WsUploadSession::Result::Rejected),
                    static_cast<int>(r));
  TEST_ASSERT_EQUAL_STRING("ERROR:Invalid START format", lastSent().c_str());
  TEST_ASSERT_FALSE(s.inProgress());
}

void test_start_negative_size_rejects() {
  WsUploadSession s(makeDeps());
  auto r = s.onStart(1, "START:a.epub:-5:/");
  TEST_ASSERT_EQUAL(static_cast<int>(WsUploadSession::Result::Rejected),
                    static_cast<int>(r));
}

void test_start_non_numeric_size_rejects() {
  WsUploadSession s(makeDeps());
  auto r = s.onStart(1, "START:a.epub:abc:/");
  TEST_ASSERT_EQUAL(static_cast<int>(WsUploadSession::Result::Rejected),
                    static_cast<int>(r));
}

void test_start_sanitizes_traversal_in_filename() {
  WsUploadSession s(makeDeps());
  s.onStart(1, "START:../../etc/passwd:10:/books");
  TEST_ASSERT_EQUAL_size_t(1, g_fake.opened.size());
  // "/" and ".." stripped; result is "etcpasswd" (slashes removed, .. removed).
  const std::string opened = g_fake.opened[0].second;
  TEST_ASSERT_EQUAL(std::string::npos, opened.find('/'));
  TEST_ASSERT_EQUAL(std::string::npos, opened.find(".."));
}

void test_start_open_failure_rejects() {
  g_fake.beginWriteOk = false;
  WsUploadSession s(makeDeps());
  auto r = s.onStart(1, "START:a.epub:10:/");
  TEST_ASSERT_EQUAL(static_cast<int>(WsUploadSession::Result::Rejected),
                    static_cast<int>(r));
  TEST_ASSERT_EQUAL_STRING("ERROR:Failed to create file", lastSent().c_str());
  TEST_ASSERT_FALSE(s.inProgress());
}

// ---- Binary frames ----

void test_binary_writes_bytes_and_completes() {
  WsUploadSession s(makeDeps());
  s.onStart(1, "START:a.epub:10:/");

  const uint8_t chunk[5] = {1, 2, 3, 4, 5};
  auto r1 = s.onBinary(1, chunk, 5);
  TEST_ASSERT_EQUAL(static_cast<int>(WsUploadSession::Result::Ok),
                    static_cast<int>(r1));

  auto r2 = s.onBinary(1, chunk, 5);
  TEST_ASSERT_EQUAL(static_cast<int>(WsUploadSession::Result::Completed),
                    static_cast<int>(r2));
  TEST_ASSERT_EQUAL(1, g_fake.closeFinalize);
  TEST_ASSERT_EQUAL(0, g_fake.closeAndDelete);
  TEST_ASSERT_EQUAL_STRING("DONE", lastSent().c_str());
  TEST_ASSERT_EQUAL_size_t(1, g_fake.cacheCleared.size());
  TEST_ASSERT_FALSE(s.inProgress());
}

void test_binary_overflow_aborts() {
  WsUploadSession s(makeDeps());
  s.onStart(1, "START:a.epub:4:/");

  const uint8_t chunk[10] = {0};
  auto r = s.onBinary(1, chunk, 10);
  TEST_ASSERT_EQUAL(static_cast<int>(WsUploadSession::Result::Aborted),
                    static_cast<int>(r));
  TEST_ASSERT_EQUAL(1, g_fake.closeAndDelete);
  TEST_ASSERT_EQUAL_STRING("ERROR:Upload overflow", lastSent().c_str());
  TEST_ASSERT_FALSE(s.inProgress());
}

void test_binary_write_failure_aborts() {
  g_fake.writeBytesOk = false;
  WsUploadSession s(makeDeps());
  s.onStart(1, "START:a.epub:10:/");

  const uint8_t chunk[5] = {0};
  auto r = s.onBinary(1, chunk, 5);
  TEST_ASSERT_EQUAL(static_cast<int>(WsUploadSession::Result::Aborted),
                    static_cast<int>(r));
  TEST_ASSERT_EQUAL_STRING("ERROR:Write failed - disk full?",
                           lastSent().c_str());
}

void test_binary_from_non_owner_rejected() {
  WsUploadSession s(makeDeps());
  s.onStart(1, "START:a.epub:10:/");

  const uint8_t chunk[5] = {0};
  auto r = s.onBinary(2, chunk, 5);  // client 2, not owner
  TEST_ASSERT_EQUAL(static_cast<int>(WsUploadSession::Result::Rejected),
                    static_cast<int>(r));
  TEST_ASSERT_TRUE(s.inProgress());  // session unaffected
}

// ---- Disconnect semantics (RFC headline race fix) ----

void test_disconnect_non_owner_is_noop() {
  WsUploadSession s(makeDeps());
  s.onStart(1, "START:a.epub:10:/");

  s.onDisconnect(99);  // some other client
  TEST_ASSERT_TRUE(s.inProgress());
  TEST_ASSERT_EQUAL(1, s.ownerClient());
  TEST_ASSERT_EQUAL(0, g_fake.closeAndDelete);
}

void test_disconnect_owner_aborts_and_resets() {
  WsUploadSession s(makeDeps());
  s.onStart(1, "START:a.epub:10:/");

  s.onDisconnect(1);
  TEST_ASSERT_FALSE(s.inProgress());
  TEST_ASSERT_EQUAL(1, g_fake.closeAndDelete);
}

void test_stale_disconnect_after_retire_is_noop() {
  // Race scenario: client A's upload completes or aborts; then a delayed
  // DISCONNECTED frame for A arrives. Must not disturb the idle state.
  WsUploadSession s(makeDeps());
  s.onStart(1, "START:a.epub:5:/");
  const uint8_t chunk[5] = {0};
  s.onBinary(1, chunk, 5);  // completes
  TEST_ASSERT_FALSE(s.inProgress());
  g_fake.closeAndDelete = 0;

  s.onDisconnect(1);  // stale event for client that no longer owns
  TEST_ASSERT_EQUAL(0, g_fake.closeAndDelete);
}

// ---- Idle timeout ----

void test_tick_noop_when_idle() {
  WsUploadSession s(makeDeps());
  s.tick(1'000'000);  // nothing happens
  TEST_ASSERT_FALSE(s.inProgress());
  TEST_ASSERT_EQUAL(0, g_fake.closeAndDelete);
}

void test_tick_before_timeout_keeps_session() {
  g_fake.now = 1000;
  WsUploadSession s(makeDeps());
  s.onStart(1, "START:a.epub:100:/");

  s.tick(1000 + WsUploadSession::kIdleTimeoutMs - 1);
  TEST_ASSERT_TRUE(s.inProgress());
}

void test_tick_at_timeout_aborts() {
  g_fake.now = 1000;
  WsUploadSession s(makeDeps());
  s.onStart(1, "START:a.epub:100:/");

  s.tick(1000 + WsUploadSession::kIdleTimeoutMs);
  TEST_ASSERT_FALSE(s.inProgress());
  TEST_ASSERT_EQUAL(1, g_fake.closeAndDelete);
  TEST_ASSERT_EQUAL_STRING("ERROR:Upload idle timeout", lastSent().c_str());
}

void test_tick_resets_on_every_binary() {
  // Receiving data refreshes the idle clock.
  g_fake.now = 1000;
  WsUploadSession s(makeDeps());
  s.onStart(1, "START:a.epub:100:/");

  g_fake.now = 20000;
  const uint8_t chunk[10] = {0};
  s.onBinary(1, chunk, 10);  // lastDataMs_ advances to 20000

  // One tick shy of timeout from the new reference.
  g_fake.now = 20000 + WsUploadSession::kIdleTimeoutMs - 1;
  s.tick(g_fake.now);
  TEST_ASSERT_TRUE(s.inProgress());
}

void test_after_timeout_new_client_can_start() {
  g_fake.now = 1000;
  WsUploadSession s(makeDeps());
  s.onStart(1, "START:a.epub:100:/");
  s.tick(1000 + WsUploadSession::kIdleTimeoutMs);  // aborts client 1

  auto r = s.onStart(2, "START:b.epub:100:/");
  TEST_ASSERT_EQUAL(static_cast<int>(WsUploadSession::Result::Ok),
                    static_cast<int>(r));
  TEST_ASSERT_EQUAL(2, s.ownerClient());
}

// ---- Snapshot ----

void test_snapshot_reports_active_state() {
  WsUploadSession s(makeDeps());
  s.onStart(1, "START:book.epub:1000:/books");

  const uint8_t chunk[100] = {0};
  s.onBinary(1, chunk, 100);

  auto snap = s.snapshot();
  TEST_ASSERT_TRUE(snap.active);
  TEST_ASSERT_EQUAL_size_t(100, snap.received);
  TEST_ASSERT_EQUAL_size_t(1000, snap.total);
  TEST_ASSERT_EQUAL_STRING("book.epub", snap.filename.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_start_valid_opens_file_and_sends_ready);
  RUN_TEST(test_start_while_active_rejects_new_client);
  RUN_TEST(test_start_bad_format_rejects);
  RUN_TEST(test_start_negative_size_rejects);
  RUN_TEST(test_start_non_numeric_size_rejects);
  RUN_TEST(test_start_sanitizes_traversal_in_filename);
  RUN_TEST(test_start_open_failure_rejects);
  RUN_TEST(test_binary_writes_bytes_and_completes);
  RUN_TEST(test_binary_overflow_aborts);
  RUN_TEST(test_binary_write_failure_aborts);
  RUN_TEST(test_binary_from_non_owner_rejected);
  RUN_TEST(test_disconnect_non_owner_is_noop);
  RUN_TEST(test_disconnect_owner_aborts_and_resets);
  RUN_TEST(test_stale_disconnect_after_retire_is_noop);
  RUN_TEST(test_tick_noop_when_idle);
  RUN_TEST(test_tick_before_timeout_keeps_session);
  RUN_TEST(test_tick_at_timeout_aborts);
  RUN_TEST(test_tick_resets_on_every_binary);
  RUN_TEST(test_after_timeout_new_client_can_start);
  RUN_TEST(test_snapshot_reports_active_state);
  return UNITY_END();
}
