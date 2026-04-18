#pragma once

#include <functional>
#include <optional>
#include <string>

#include "Nav.h"

class Activity;

namespace lifecycle {

// ActivityRouter — owns activity transitions, per-route persist/trim policy,
// and deep-sleep entry sequencing. Activities continue to receive std::function
// callbacks; main.cpp wraps this router for the V2 path.
class ActivityRouter {
 public:
  static ActivityRouter& instance();

  // Synchronous boot dispatch — caller expects activity live on return.
  // Applies RoutePolicy + runs factory directly (no queueing).
  void begin(const Nav& initial);

  // Queue a transition; drained at safe boundary by applyIfPending().
  void request(const Nav& nav);

  // Called once per loop() tick after currentActivity->loop(). Drains pending.
  void applyIfPending();

  // Orchestrate deep-sleep entry in fixed order:
  //   onBeforeDeepSleep(fromReader)  -> BLE teardown + set lastSleepFromReader
  //   persistAppState("enter deep sleep")
  //   exit current activity
  //   enterSleepActivity               -> construct + enter SleepActivity
  //   onAfterDeepSleep                 -> display.deepSleep + startDeepSleep (no return)
  void enterDeepSleep(bool fromReader);

  // main.cpp owns activity construction (holds renderer, mappedInputManager,
  // onGoX globals). It registers one factory per route.
  using Factory = std::function<void(const std::string& payload)>;
  void setRouteFactory(RouteId route, Factory factory);

  std::function<void(const std::string&)> makeGoToReader() const;
  std::function<void()> makeGoHome() const;
  std::function<void()> makeGoToSettings() const;
  std::function<void()> makeGoToMyLibrary() const;
  std::function<void(const std::string&)> makeGoToMyLibraryWithPath() const;
  std::function<void()> makeGoToRecentBooks() const;
  std::function<void()> makeGoToFileTransfer() const;
  std::function<void()> makeGoToBrowser() const;

  // Injection seam for main.cpp (production) and host tests (stubs).
  // All side-effecting calls flow through these — router itself never touches
  // SD, display, GPIO, or global APP_STATE directly.
  struct Deps {
    Activity** currentActivitySlot = nullptr;
    bool (*persistAppState)(const char* ctx) = nullptr;
    void (*trimSleepFolderIfDirty)() = nullptr;
    void (*onBeforeDeepSleep)(bool fromReader) = nullptr;
    void (*onAfterDeepSleep)() = nullptr;
    std::function<void()> enterSleepActivity;
  };
  void setDeps(Deps deps);
  static void setDepsForTest(Deps deps);

#ifdef UNIT_TEST_HOST
  // Clears deps, factories, and pending nav on the singleton. Host tests only.
  static void resetForTest();
#endif

 private:
  ActivityRouter() = default;
  void dispatch(const Nav& nav);
  static constexpr size_t kRouteCount = 8;
  std::optional<Nav> pending_;
  bool busy_ = false;
  Deps deps_{};
  Factory factories_[kRouteCount];
};

}  // namespace lifecycle
