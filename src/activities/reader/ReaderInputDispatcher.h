// ReaderInputDispatcher — the reader's input decision FSM as a pure module
// (RFC #165, step 1).
//
// Today EpubReaderActivity::loop()/loopPageTurn() decode button edges + the
// clock + settings + mode into an action, inline, welded to millis()/
// MappedInputManager/render state. That timing logic (350 ms double-tap,
// 1000 ms long-press, 700 ms chapter-skip, the press-vs-release polarity, the
// Power+Down screenshot chord, end-of-book) is device-only and untestable.
//
// This core takes a per-frame snapshot (edges + nowMs + heldTime), settings,
// and the activity's mode/state, and returns ONE decoded action plus a
// suppress-input effect. The activity keeps the impure execution (RenderLock,
// section work, navigation). Includes only <cstdint> + the ReaderButton mirror
// — no Arduino, FreeRTOS, MappedInputManager, or CrossPointSettings — so it
// compiles and unit-tests on the host.
//
// Step 1 is additive: nothing is wired into the readers yet, so device
// behaviour is unchanged. The thresholds and decode order are copied verbatim
// from EpubReaderActivity; step 2 swaps the activity onto this core.
#pragma once

#include <cstdint>

#include "ReaderButton.h"

namespace crosspoint {
namespace reader {

// One frame of input, snapshotted from MappedInputManager by the adapter.
struct ReaderInput {
  bool pressed[kReaderButtonCount] = {};   // wasPressed(b)
  bool released[kReaderButtonCount] = {};   // wasReleased(b)
  bool down[kReaderButtonCount] = {};       // isPressed(b) (level)
  bool anyPressed = false;                   // wasAnyPressed()
  bool anyReleased = false;                  // wasAnyReleased()
  unsigned long heldTimeMs = 0;              // getHeldTime() (GLOBAL, not per-button)
  unsigned long nowMs = 0;                   // millis()

  bool p(ReaderButton b) const { return pressed[static_cast<int>(b)]; }
  bool r(ReaderButton b) const { return released[static_cast<int>(b)]; }
  bool d(ReaderButton b) const { return down[static_cast<int>(b)]; }
};

// Settings collapsed to the booleans the core branches on (no CrossPointSettings).
struct ReaderInputSettings {
  bool longPressChapterSkip = true;  // true => page turn on RELEASE; false => on PRESS
  bool powerIsPageTurn = false;      // SETTINGS.shortPwrBtn == PAGE_TURN
};

// Pure activity state the decision reads (no epub/section/HighlightController).
struct ReaderState {
  enum class Mode : uint8_t { Normal, Recovery, Highlight };
  Mode mode = Mode::Normal;
  bool inFootnote = false;    // footnoteDepth > 0  -> Back restores
  bool atEndOfBook = false;   // currentSpineIndex > 0 && >= spineItemsCount
  bool hasSection = false;    // section != nullptr
};

// Per-reader gesture opt-in (epub: all on; txt/xtc: leaner). OpenMenu and
// LongPressConfirm are semantic slots each reader maps to its own action.
struct ReaderInputConfig {
  bool doubleTapToggle = true;   // Confirm double-tap -> ToggleTextRenderMode
  bool longPressConfirm = true;  // Confirm long-press -> LongPressConfirm
  bool footnoteBack = false;     // Back restores footnote when inFootnote (epub)
  bool chapterSkip = false;      // held page button -> SkipChapter* (epub + xtc)
};

enum class ReaderAction : uint8_t {
  None,
  GoHome,
  RestoreFootnote,
  OpenMenu,
  ToggleTextRenderMode,
  LongPressConfirm,
  PagePrev,
  PageNext,
  SkipChapterPrev,
  SkipChapterNext,
  EndOfBookGoHome,
  EndOfBookStay,
  ExitRecoveryToHome,
  ExitRecoveryRetry,
  RequestUpdate,
};

class ReaderInputDispatcher {
 public:
  // Thresholds — verbatim from EpubReaderActivity (confirmDoubleTapMs / goHomeMs
  // / skipChapterMs).
  static constexpr unsigned long kConfirmDoubleTapMs = 350;
  static constexpr unsigned long kGoHomeMs = 1000;
  static constexpr unsigned long kSkipChapterMs = 700;

  struct Result {
    ReaderAction action = ReaderAction::None;
    bool suppressUntilAllReleased = false;  // adapter performs the suppress
  };

  explicit ReaderInputDispatcher(ReaderInputConfig cfg = {}) : cfg_(cfg) {}

  // Pure: a function of (input, settings, state) plus the dispatcher's own
  // cross-frame latches. No I/O, no clock read.
  Result dispatch(const ReaderInput& in, const ReaderInputSettings& s, const ReaderState& st) {
    // --- Modal gates (decode order mirrors loop()) ---
    if (st.mode == ReaderState::Mode::Recovery) {
      if (in.p(ReaderButton::Back)) return {ReaderAction::ExitRecoveryToHome, false};
      if (in.anyReleased) return {ReaderAction::ExitRecoveryRetry, false};
      return {};
    }
    if (st.mode == ReaderState::Mode::Highlight) {
      return {};  // highlight has its own loop; the dispatcher yields
    }

    const bool confirmDown = in.d(ReaderButton::Confirm);

    // 1. Deferred single-tap menu open: window expired with Confirm up.
    if (cfg_.doubleTapToggle && pendingMenuOpen_ && !confirmDown &&
        in.nowMs - lastConfirmReleaseMs_ > kConfirmDoubleTapMs) {
      pendingMenuOpen_ = false;
      return {ReaderAction::OpenMenu, false};
    }

    // 2. Long-press latch reset.
    if (!confirmDown) confirmLongPressHandled_ = false;

    // 3. Confirm release: arm menu, or second tap toggles render mode.
    if (in.r(ReaderButton::Confirm)) {
      if (!cfg_.doubleTapToggle) {
        return {ReaderAction::OpenMenu, false};  // no double-tap: immediate
      }
      if (pendingMenuOpen_ && in.nowMs - lastConfirmReleaseMs_ <= kConfirmDoubleTapMs) {
        pendingMenuOpen_ = false;
        return {ReaderAction::ToggleTextRenderMode, false};
      }
      pendingMenuOpen_ = true;
      lastConfirmReleaseMs_ = in.nowMs;
      return {};
    }

    // 4. Confirm long-press -> highlight (requests input suppression), once.
    if (cfg_.longPressConfirm && !confirmLongPressHandled_ && confirmDown &&
        in.heldTimeMs >= kGoHomeMs) {
      confirmLongPressHandled_ = true;
      return {ReaderAction::LongPressConfirm, true};
    }

    // 5. Back: footnote restore else go home.
    if (in.p(ReaderButton::Back)) {
      if (cfg_.footnoteBack && st.inFootnote) return {ReaderAction::RestoreFootnote, false};
      return {ReaderAction::GoHome, false};
    }

    // 6. Page-turn triggers (press-vs-release per setting; power-as-next).
    const bool usePress = !s.longPressChapterSkip;
    const bool powerTurn = s.powerIsPageTurn && in.r(ReaderButton::Power);
    const bool prev = usePress ? (in.p(ReaderButton::PageBack) || in.p(ReaderButton::Left))
                               : (in.r(ReaderButton::PageBack) || in.r(ReaderButton::Left));
    const bool next = usePress ? (in.p(ReaderButton::PageForward) || powerTurn || in.p(ReaderButton::Right))
                               : (in.r(ReaderButton::PageForward) || powerTurn || in.r(ReaderButton::Right));
    if (!prev && !next) return {};

    // --- loopPageTurn decision (impure execution stays in the activity) ---
    if (st.atEndOfBook) {
      return {next ? ReaderAction::EndOfBookGoHome : ReaderAction::EndOfBookStay, false};
    }
    // Screenshot chord: Power+Down both released swallows the turn.
    if (in.r(ReaderButton::Power) && in.r(ReaderButton::Down)) return {};
    // Chapter skip on a long hold (gated per-reader).
    if (cfg_.chapterSkip && s.longPressChapterSkip && in.heldTimeMs > kSkipChapterMs) {
      return {next ? ReaderAction::SkipChapterNext : ReaderAction::SkipChapterPrev, false};
    }
    // No content loaded: any press forces a rebuild render.
    if (!st.hasSection) {
      return in.anyPressed ? Result{ReaderAction::RequestUpdate, false} : Result{};
    }
    // Normal page turn. prev takes precedence when both fire in one frame,
    // matching loopPageTurn's `if (prevTriggered) {...} else {...}`.
    return {prev ? ReaderAction::PagePrev : ReaderAction::PageNext, false};
  }

  // Clear cross-frame latches (activity enter / mode exit).
  void reset() {
    pendingMenuOpen_ = false;
    lastConfirmReleaseMs_ = 0;
    confirmLongPressHandled_ = false;
  }

  // Surgically clear just the deferred single-tap, exactly like the old
  // `pendingMenuOpen = false`. Used by the menu/subactivity-exit sites so a
  // return from a menu doesn't re-fire the pending tap as a fresh menu-open.
  void clearPendingTap() { pendingMenuOpen_ = false; }

  // Inspectors (mirror HighlightController::state() for tests).
  bool pendingMenuOpen() const { return pendingMenuOpen_; }
  bool confirmLongPressHandled() const { return confirmLongPressHandled_; }

 private:
  ReaderInputConfig cfg_;
  // The FSM memory, migrated out of EpubReaderActivity's members.
  bool pendingMenuOpen_ = false;
  unsigned long lastConfirmReleaseMs_ = 0;
  bool confirmLongPressHandled_ = false;
};

}  // namespace reader
}  // namespace crosspoint
