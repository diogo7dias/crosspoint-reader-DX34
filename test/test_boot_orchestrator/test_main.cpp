// Host-side tests for the BootSequenceOrchestrator pure core (RFC #166, step 1).
//
// The boot-destination decision (silent-reboot routing, the crash-loop guard,
// the .epub filter, the random/most-recent pick, and the durable-guard-bump
// decision) is the most safety-critical logic in the firmware and is device-only
// today. These tests pin every branch with synthetic BootInputs + a scripted
// random, no hardware.
//
// Run via: pio test -e test_host -f test_boot_orchestrator
#include <unity.h>

#include "boot/BootSequenceOrchestrator.h"

using crosspoint::boot::BootDecision;
using crosspoint::boot::BootInputs;
using crosspoint::boot::decideBoot;

namespace {

// Scripted random: records the count it was asked for, returns g_rngReturn.
uint32_t g_rngReturn = 0;
uint32_t g_rngLastCount = 0;
uint32_t testRng(uint32_t count) {
  g_rngLastCount = count;
  return g_rngReturn;
}

BootInputs normalBoot() {
  BootInputs in;
  in.isSilentReboot = false;
  in.silentRebootTarget = -1;
  in.readerActivityLoadCount = 0;
  in.backHeld = false;
  in.randomBookOnBoot = false;
  return in;
}

}  // namespace

void setUp() {
  g_rngReturn = 0;
  g_rngLastCount = 0;
}
void tearDown() {}

// ── Crash-loop guard + Back-held ───────────────────────────────────────────

void test_crash_loop_guard_forces_home_no_bump() {
  BootInputs in = normalBoot();
  in.readerActivityLoadCount = 1;  // the brick-class guard
  in.recentBookPaths = {"/books/a.epub"};
  BootDecision d = decideBoot(in, testRng);
  TEST_ASSERT_TRUE(d.goHome);
  TEST_ASSERT_FALSE(d.bumpGuard);
}

void test_back_held_forces_home() {
  BootInputs in = normalBoot();
  in.backHeld = true;
  in.recentBookPaths = {"/books/a.epub"};
  BootDecision d = decideBoot(in, testRng);
  TEST_ASSERT_TRUE(d.goHome);
  TEST_ASSERT_FALSE(d.bumpGuard);
}

// ── Normal reader launch bumps the durable guard ───────────────────────────

void test_normal_reader_launch_bumps_guard() {
  BootInputs in = normalBoot();
  in.recentBookPaths = {"/books/recent.epub", "/books/old.epub"};
  BootDecision d = decideBoot(in, testRng);
  TEST_ASSERT_FALSE(d.goHome);
  TEST_ASSERT_EQUAL_STRING("/books/recent.epub", d.readerPath.c_str());
  TEST_ASSERT_TRUE(d.bumpGuard);
}

void test_no_epub_recents_goes_home() {
  BootInputs in = normalBoot();
  in.recentBookPaths = {"/notes.txt", "/manual.pdf"};
  BootDecision d = decideBoot(in, testRng);
  TEST_ASSERT_TRUE(d.goHome);
  TEST_ASSERT_FALSE(d.bumpGuard);
}

// ── Silent-reboot routing (never bumps) ────────────────────────────────────

void test_silent_reader_resumes_open_epub() {
  BootInputs in = normalBoot();
  in.isSilentReboot = true;
  in.silentRebootTarget = 1;  // reader
  in.openEpubPath = "/books/a.epub";
  BootDecision d = decideBoot(in, testRng);
  TEST_ASSERT_FALSE(d.goHome);
  TEST_ASSERT_EQUAL_STRING("/books/a.epub", d.readerPath.c_str());
  TEST_ASSERT_FALSE(d.bumpGuard);  // silent never bumps
}

void test_silent_reader_empty_path_goes_home() {
  BootInputs in = normalBoot();
  in.isSilentReboot = true;
  in.silentRebootTarget = 1;
  in.openEpubPath = "";
  BootDecision d = decideBoot(in, testRng);
  TEST_ASSERT_TRUE(d.goHome);
  TEST_ASSERT_FALSE(d.bumpGuard);
}

void test_silent_home_target_goes_home() {
  BootInputs in = normalBoot();
  in.isSilentReboot = true;
  in.silentRebootTarget = 0;          // home
  in.openEpubPath = "/books/a.epub";  // ignored on a home-target silent reboot
  BootDecision d = decideBoot(in, testRng);
  TEST_ASSERT_TRUE(d.goHome);
}

// ── Random pick operates on the FILTERED epub count ────────────────────────

void test_random_pick_uses_filtered_count() {
  BootInputs in = normalBoot();
  in.randomBookOnBoot = true;
  in.recentBookPaths = {"/a.epub", "/notes.txt", "/b.epub", "/c.epub"};  // 3 epubs after filter
  g_rngReturn = 2;
  BootDecision d = decideBoot(in, testRng);
  TEST_ASSERT_EQUAL_UINT32(3, g_rngLastCount);  // filter applied BEFORE random
  TEST_ASSERT_EQUAL_STRING("/c.epub", d.readerPath.c_str());
  TEST_ASSERT_TRUE(d.bumpGuard);
}

void test_random_off_multiple_epubs_takes_front() {
  BootInputs in = normalBoot();
  in.randomBookOnBoot = false;
  in.recentBookPaths = {"/a.epub", "/b.epub"};
  BootDecision d = decideBoot(in, testRng);
  TEST_ASSERT_EQUAL_STRING("/a.epub", d.readerPath.c_str());
}

void test_random_on_single_epub_takes_front_without_rng() {
  BootInputs in = normalBoot();
  in.randomBookOnBoot = true;
  in.recentBookPaths = {"/only.epub"};
  BootDecision d = decideBoot(in, testRng);
  TEST_ASSERT_EQUAL_STRING("/only.epub", d.readerPath.c_str());
  TEST_ASSERT_EQUAL_UINT32(0, g_rngLastCount);  // rng never consulted (count == 1)
}

// ── .epub filter edge cases ────────────────────────────────────────────────

void test_epub_filter_rejects_bare_and_uppercase() {
  BootInputs in = normalBoot();
  // ".epub" is exactly 5 chars (fails size>5); "/c.EPUB" fails case. Only the
  // real ".epub" qualifies, even though it isn't first in recents.
  in.recentBookPaths = {".epub", "/c.EPUB", "/real.epub"};
  BootDecision d = decideBoot(in, testRng);
  TEST_ASSERT_FALSE(d.goHome);
  TEST_ASSERT_EQUAL_STRING("/real.epub", d.readerPath.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_crash_loop_guard_forces_home_no_bump);
  RUN_TEST(test_back_held_forces_home);
  RUN_TEST(test_normal_reader_launch_bumps_guard);
  RUN_TEST(test_no_epub_recents_goes_home);
  RUN_TEST(test_silent_reader_resumes_open_epub);
  RUN_TEST(test_silent_reader_empty_path_goes_home);
  RUN_TEST(test_silent_home_target_goes_home);
  RUN_TEST(test_random_pick_uses_filtered_count);
  RUN_TEST(test_random_off_multiple_epubs_takes_front);
  RUN_TEST(test_random_on_single_epub_takes_front_without_rng);
  RUN_TEST(test_epub_filter_rejects_bare_and_uppercase);
  return UNITY_END();
}
