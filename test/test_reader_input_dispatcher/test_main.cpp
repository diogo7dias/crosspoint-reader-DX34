// Host-side tests for the ReaderInputDispatcher pure core (RFC #165, step 1).
//
// The dispatcher decodes one frame of input edges + a clock + settings + mode
// into a single ReaderAction (the decision), leaving the impure execution
// (RenderLock, section work, navigation) to the activity. These tests pin the
// exact tap/double-tap/long-press/page-turn semantics that are device-only
// today — driven by a scripted nowMs and synthetic edge frames, no hardware.
//
// Run via: pio test -e test_host -f test_reader_input_dispatcher
#include <unity.h>

#include "activities/reader/ReaderInputDispatcher.h"

using crosspoint::reader::ReaderAction;
using crosspoint::reader::ReaderButton;
using crosspoint::reader::ReaderInput;
using crosspoint::reader::ReaderInputConfig;
using crosspoint::reader::ReaderInputDispatcher;
using crosspoint::reader::ReaderInputSettings;
using crosspoint::reader::ReaderState;

namespace {

// Epub config: all gestures on. (Default ctor is the leaner txt/xtc shape.)
ReaderInputConfig epubCfg() {
  ReaderInputConfig c;
  c.doubleTapToggle = true;
  c.longPressConfirm = true;
  c.footnoteBack = true;
  c.chapterSkip = true;
  return c;
}

// Default settings: page turn on RELEASE (longPressChapterSkip == true), no power turn.
ReaderInputSettings relSettings() { return ReaderInputSettings{true, false}; }
ReaderInputSettings pressSettings() { return ReaderInputSettings{false, false}; }

ReaderState normalReading() {
  ReaderState s;
  s.mode = ReaderState::Mode::Normal;
  s.hasSection = true;
  return s;
}

ReaderInput frame(unsigned long nowMs) {
  ReaderInput f;
  f.nowMs = nowMs;
  return f;
}
void press(ReaderInput& f, ReaderButton b) {
  f.pressed[static_cast<int>(b)] = true;
  f.anyPressed = true;
}
void release(ReaderInput& f, ReaderButton b) {
  f.released[static_cast<int>(b)] = true;
  f.anyReleased = true;
}
void hold(ReaderInput& f, ReaderButton b, unsigned long heldMs) {
  f.down[static_cast<int>(b)] = true;
  f.heldTimeMs = heldMs;
}

int act(const ReaderInputDispatcher::Result& r) { return static_cast<int>(r.action); }

}  // namespace

void setUp() {}
void tearDown() {}

// ── Confirm: tap / double-tap / long-press ─────────────────────────────────

void test_double_tap_toggles_render_mode() {
  ReaderInputDispatcher d(epubCfg());
  ReaderInput f1 = frame(1000);
  release(f1, ReaderButton::Confirm);
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::None, act(d.dispatch(f1, relSettings(), normalReading())));  // arms
  TEST_ASSERT_TRUE(d.pendingMenuOpen());

  ReaderInput f2 = frame(1200);  // 200ms <= 350 double-tap window
  release(f2, ReaderButton::Confirm);
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::ToggleTextRenderMode,
                        act(d.dispatch(f2, relSettings(), normalReading())));
  TEST_ASSERT_FALSE(d.pendingMenuOpen());
}

void test_single_tap_opens_menu_after_window() {
  ReaderInputDispatcher d(epubCfg());
  ReaderInput f1 = frame(1000);
  release(f1, ReaderButton::Confirm);
  d.dispatch(f1, relSettings(), normalReading());  // arm
  ReaderInput f2 = frame(1400);                    // 400ms > 350, Confirm not held
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::OpenMenu, act(d.dispatch(f2, relSettings(), normalReading())));
}

void test_long_press_enters_highlight_and_suppresses_once() {
  ReaderInputDispatcher d(epubCfg());
  ReaderInput held = frame(5000);
  hold(held, ReaderButton::Confirm, 1000);  // >= goHomeMs
  auto r = d.dispatch(held, relSettings(), normalReading());
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::LongPressConfirm, act(r));
  TEST_ASSERT_TRUE(r.suppressUntilAllReleased);
  // latched: still held, must not refire
  ReaderInput held2 = frame(5500);
  hold(held2, ReaderButton::Confirm, 1500);
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::None, act(d.dispatch(held2, relSettings(), normalReading())));
}

void test_long_press_disabled_by_config() {
  ReaderInputConfig c = epubCfg();
  c.longPressConfirm = false;
  ReaderInputDispatcher d(c);
  ReaderInput held = frame(5000);
  hold(held, ReaderButton::Confirm, 1500);
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::None, act(d.dispatch(held, relSettings(), normalReading())));
}

void test_single_tap_immediate_menu_when_double_tap_disabled() {
  ReaderInputConfig c = epubCfg();
  c.doubleTapToggle = false;
  ReaderInputDispatcher d(c);
  ReaderInput f = frame(1000);
  release(f, ReaderButton::Confirm);
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::OpenMenu, act(d.dispatch(f, relSettings(), normalReading())));
}

// ── Back: footnote vs home ─────────────────────────────────────────────────

void test_back_goes_home_in_normal() {
  ReaderInputDispatcher d(epubCfg());
  ReaderInput f = frame(0);
  press(f, ReaderButton::Back);
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::GoHome, act(d.dispatch(f, relSettings(), normalReading())));
}

void test_back_restores_footnote_when_in_footnote() {
  ReaderInputDispatcher d(epubCfg());
  ReaderState st = normalReading();
  st.inFootnote = true;
  ReaderInput f = frame(0);
  press(f, ReaderButton::Back);
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::RestoreFootnote, act(d.dispatch(f, relSettings(), st)));
}

// ── Page turns: polarity, power, screenshot ────────────────────────────────

void test_page_next_on_release_in_release_mode() {
  ReaderInputDispatcher d(epubCfg());
  ReaderInput f = frame(0);
  release(f, ReaderButton::PageForward);
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::PageNext, act(d.dispatch(f, relSettings(), normalReading())));
}

void test_page_prev_on_release_in_release_mode() {
  ReaderInputDispatcher d(epubCfg());
  ReaderInput f = frame(0);
  release(f, ReaderButton::PageBack);
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::PagePrev, act(d.dispatch(f, relSettings(), normalReading())));
}

void test_press_edge_mode_turns_on_press_not_release() {
  ReaderInputDispatcher d(epubCfg());
  ReaderInput pr = frame(0);
  press(pr, ReaderButton::Right);
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::PageNext, act(d.dispatch(pr, pressSettings(), normalReading())));
  ReaderInput rel = frame(0);
  release(rel, ReaderButton::Right);
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::None, act(d.dispatch(rel, pressSettings(), normalReading())));
}

void test_power_page_turn_when_enabled() {
  ReaderInputDispatcher d(epubCfg());
  ReaderInputSettings s{true, true};  // powerIsPageTurn
  ReaderInput f = frame(0);
  release(f, ReaderButton::Power);
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::PageNext, act(d.dispatch(f, s, normalReading())));
}

void test_power_down_chord_suppresses_turn() {
  ReaderInputDispatcher d(epubCfg());
  ReaderInputSettings s{true, true};
  ReaderInput f = frame(0);
  release(f, ReaderButton::Power);
  release(f, ReaderButton::Down);
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::None, act(d.dispatch(f, s, normalReading())));
}

// ── Chapter skip ───────────────────────────────────────────────────────────

void test_chapter_skip_on_long_hold() {
  ReaderInputDispatcher d(epubCfg());
  ReaderInput f = frame(0);
  release(f, ReaderButton::PageForward);
  hold(f, ReaderButton::PageForward, 800);  // > 700 skipChapterMs
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::SkipChapterNext, act(d.dispatch(f, relSettings(), normalReading())));
}

void test_chapter_skip_disabled_falls_back_to_page_turn() {
  ReaderInputConfig c = epubCfg();
  c.chapterSkip = false;
  ReaderInputDispatcher d(c);
  ReaderInput f = frame(0);
  release(f, ReaderButton::PageForward);
  hold(f, ReaderButton::PageForward, 800);
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::PageNext, act(d.dispatch(f, relSettings(), normalReading())));
}

// ── End of book ────────────────────────────────────────────────────────────

void test_end_of_book_next_goes_home() {
  ReaderInputDispatcher d(epubCfg());
  ReaderState st = normalReading();
  st.atEndOfBook = true;
  ReaderInput f = frame(0);
  release(f, ReaderButton::PageForward);
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::EndOfBookGoHome, act(d.dispatch(f, relSettings(), st)));
}

void test_end_of_book_prev_stays() {
  ReaderInputDispatcher d(epubCfg());
  ReaderState st = normalReading();
  st.atEndOfBook = true;
  ReaderInput f = frame(0);
  release(f, ReaderButton::PageBack);
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::EndOfBookStay, act(d.dispatch(f, relSettings(), st)));
}

// ── Modes: recovery, highlight, no-section ─────────────────────────────────

void test_recovery_back_exits_to_home() {
  ReaderInputDispatcher d(epubCfg());
  ReaderState st;
  st.mode = ReaderState::Mode::Recovery;
  ReaderInput f = frame(0);
  press(f, ReaderButton::Back);
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::ExitRecoveryToHome, act(d.dispatch(f, relSettings(), st)));
}

void test_recovery_any_release_retries() {
  ReaderInputDispatcher d(epubCfg());
  ReaderState st;
  st.mode = ReaderState::Mode::Recovery;
  ReaderInput f = frame(0);
  release(f, ReaderButton::Confirm);
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::ExitRecoveryRetry, act(d.dispatch(f, relSettings(), st)));
}

void test_highlight_mode_yields_none() {
  ReaderInputDispatcher d(epubCfg());
  ReaderState st = normalReading();
  st.mode = ReaderState::Mode::Highlight;
  ReaderInput f = frame(0);
  press(f, ReaderButton::Confirm);
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::None, act(d.dispatch(f, relSettings(), st)));
}

void test_no_section_press_requests_update() {
  ReaderInputDispatcher d(epubCfg());
  ReaderState st = normalReading();
  st.hasSection = false;
  ReaderInput f = frame(0);
  press(f, ReaderButton::Right);  // press-edge trigger so anyPressed is set
  TEST_ASSERT_EQUAL_INT((int)ReaderAction::RequestUpdate, act(d.dispatch(f, pressSettings(), st)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_double_tap_toggles_render_mode);
  RUN_TEST(test_single_tap_opens_menu_after_window);
  RUN_TEST(test_long_press_enters_highlight_and_suppresses_once);
  RUN_TEST(test_long_press_disabled_by_config);
  RUN_TEST(test_single_tap_immediate_menu_when_double_tap_disabled);
  RUN_TEST(test_back_goes_home_in_normal);
  RUN_TEST(test_back_restores_footnote_when_in_footnote);
  RUN_TEST(test_page_next_on_release_in_release_mode);
  RUN_TEST(test_page_prev_on_release_in_release_mode);
  RUN_TEST(test_press_edge_mode_turns_on_press_not_release);
  RUN_TEST(test_power_page_turn_when_enabled);
  RUN_TEST(test_power_down_chord_suppresses_turn);
  RUN_TEST(test_chapter_skip_on_long_hold);
  RUN_TEST(test_chapter_skip_disabled_falls_back_to_page_turn);
  RUN_TEST(test_end_of_book_next_goes_home);
  RUN_TEST(test_end_of_book_prev_stays);
  RUN_TEST(test_recovery_back_exits_to_home);
  RUN_TEST(test_recovery_any_release_retries);
  RUN_TEST(test_highlight_mode_yields_none);
  RUN_TEST(test_no_section_press_requests_update);
  return UNITY_END();
}
