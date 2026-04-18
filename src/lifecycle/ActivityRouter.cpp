#include "ActivityRouter.h"

#include "../activities/Activity.h"

namespace lifecycle {

ActivityRouter& ActivityRouter::instance() {
  static ActivityRouter s_instance;
  return s_instance;
}

void ActivityRouter::begin(const Nav& initial) {
  dispatch(initial);
}

void ActivityRouter::request(const Nav& nav) {
  pending_ = nav;
}

void ActivityRouter::applyIfPending() {
  if (!pending_ || busy_) return;
  const Nav nav = *pending_;
  pending_.reset();
  dispatch(nav);
}

void ActivityRouter::dispatch(const Nav& nav) {
  const size_t idx = static_cast<size_t>(nav.route);
  if (idx >= kRouteCount) return;
  auto& factory = factories_[idx];
  if (!factory) return;  // Route not migrated — caller must use legacy path.

  busy_ = true;
  const RoutePolicy p = policyFor(nav.route);
  if (p.trimSleepFolder && deps_.trimSleepFolderIfDirty) {
    deps_.trimSleepFolderIfDirty();
  }
  if (p.persistBefore && p.persistCtx && deps_.persistAppState) {
    deps_.persistAppState(p.persistCtx);
  }
  factory(nav.payload);
  busy_ = false;
}

void ActivityRouter::setRouteFactory(RouteId route, Factory factory) {
  const size_t idx = static_cast<size_t>(route);
  if (idx >= kRouteCount) return;
  factories_[idx] = std::move(factory);
}

void ActivityRouter::enterDeepSleep(bool fromReader) {
  // Fixed ordering — test harness asserts this exact sequence.
  if (deps_.onBeforeDeepSleep) deps_.onBeforeDeepSleep(fromReader);
  if (deps_.persistAppState) deps_.persistAppState("enter deep sleep");

  if (deps_.currentActivitySlot) {
    Activity*& slot = *deps_.currentActivitySlot;
    if (slot) {
      slot->onExit();
      delete slot;
      slot = nullptr;
    }
  }

  if (deps_.enterSleepActivity) deps_.enterSleepActivity();

  // Does not return in production (startDeepSleep halts the CPU).
  if (deps_.onAfterDeepSleep) deps_.onAfterDeepSleep();
}

// Lambda synthesizers — still empty stubs. Separate PR wires these to
// request() and drops the legacy free functions in main.cpp.
std::function<void(const std::string&)> ActivityRouter::makeGoToReader() const {
  return [](const std::string&) {};
}
std::function<void()> ActivityRouter::makeGoHome() const { return [] {}; }
std::function<void()> ActivityRouter::makeGoToSettings() const { return [] {}; }
std::function<void()> ActivityRouter::makeGoToMyLibrary() const { return [] {}; }
std::function<void(const std::string&)> ActivityRouter::makeGoToMyLibraryWithPath() const {
  return [](const std::string&) {};
}
std::function<void()> ActivityRouter::makeGoToRecentBooks() const { return [] {}; }
std::function<void()> ActivityRouter::makeGoToFileTransfer() const { return [] {}; }
std::function<void()> ActivityRouter::makeGoToBrowser() const { return [] {}; }

void ActivityRouter::setDeps(Deps deps) { deps_ = std::move(deps); }

void ActivityRouter::setDepsForTest(Deps deps) { instance().deps_ = std::move(deps); }

}  // namespace lifecycle
