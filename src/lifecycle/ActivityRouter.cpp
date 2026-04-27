#include "ActivityRouter.h"

#ifdef UNIT_TEST_HOST
#include "ActivityStubForHostTest.h"
#else
#include "../activities/Activity.h"
#endif

namespace lifecycle {

ActivityRouter& ActivityRouter::instance() {
  static ActivityRouter s_instance;
  return s_instance;
}

void ActivityRouter::begin(const Nav& initial) { dispatch(initial); }

void ActivityRouter::request(const Nav& nav) { pending_ = nav; }

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

  // Second flush: captures dirty state written during onExit() before the CPU
  // halts. Without this, flushSoon() calls in onExit() (e.g. resetting
  // readerActivityLoadCount to 0) are never drained — the debounce window
  // never expires once deep sleep starts, so the stale value survives to the
  // next boot and triggers the crash-loop guard, sending the user to home
  // instead of resuming the book they were reading.
  if (deps_.persistAppState) deps_.persistAppState("after activity exit");

  if (deps_.enterSleepActivity) deps_.enterSleepActivity();

  // Does not return in production (startDeepSleep halts the CPU).
  if (deps_.onAfterDeepSleep) deps_.onAfterDeepSleep();
}

// Lambda synthesizers — return std::function callbacks bound to router.request.
// Activities continue to receive the callbacks they have always received;
// these let a caller (production main.cpp, or a host test) swap out the source
// without touching every activity constructor.
std::function<void(const std::string&)> ActivityRouter::makeGoToReader() const {
  return [](const std::string& p) { instance().request({RouteId::Reader, p}); };
}
std::function<void()> ActivityRouter::makeGoHome() const {
  return [] { instance().request({RouteId::Home, ""}); };
}
std::function<void()> ActivityRouter::makeGoToSettings() const {
  return [] { instance().request({RouteId::Settings, ""}); };
}
std::function<void()> ActivityRouter::makeGoToMyLibrary() const {
  return [] { instance().request({RouteId::MyLibrary, ""}); };
}
std::function<void(const std::string&)> ActivityRouter::makeGoToMyLibraryWithPath() const {
  return [](const std::string& p) { instance().request({RouteId::MyLibraryAt, p}); };
}
std::function<void()> ActivityRouter::makeGoToRecentBooks() const {
  return [] { instance().request({RouteId::RecentBooks, ""}); };
}
std::function<void()> ActivityRouter::makeGoToFileTransfer() const {
  return [] { instance().request({RouteId::FileTransfer, ""}); };
}
std::function<void()> ActivityRouter::makeGoToBrowser() const {
  return [] { instance().request({RouteId::Browser, ""}); };
}

void ActivityRouter::setDeps(Deps deps) { deps_ = std::move(deps); }

void ActivityRouter::setDepsForTest(Deps deps) { instance().deps_ = std::move(deps); }

#ifdef UNIT_TEST_HOST
void ActivityRouter::resetForTest() {
  auto& r = instance();
  r.deps_ = Deps{};
  for (size_t i = 0; i < kRouteCount; ++i) r.factories_[i] = Factory{};
  r.pending_.reset();
  r.busy_ = false;
}
#endif

}  // namespace lifecycle
