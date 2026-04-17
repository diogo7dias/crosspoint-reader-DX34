#include "ActivityRouter.h"

namespace lifecycle {

ActivityRouter& ActivityRouter::instance() {
  static ActivityRouter s_instance;
  return s_instance;
}

void ActivityRouter::begin(const Nav& /*initial*/) {
  // Scaffold: no-op. Follow-up PR wires setup() dispatch here.
}

void ActivityRouter::request(const Nav& nav) {
  // Scaffold: coalesce only. No side effects; applyIfPending drains.
  pending_ = nav;
}

void ActivityRouter::applyIfPending() {
  // Scaffold: no-op. Follow-up PR performs the transition.
  if (!pending_) return;
  pending_.reset();
}

void ActivityRouter::enterDeepSleep(bool /*fromReader*/) {
  // Scaffold: no-op. main.cpp still owns the 7-step deep-sleep sequence
  // until the dedicated deep-sleep migration PR lands.
}

// Lambda synthesizers — return empty lambdas for now so callers can opt in
// incrementally. main.cpp keeps using its own onGo* lambdas until migration.
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

void ActivityRouter::setDepsForTest(Deps deps) {
  instance().deps_ = deps;
}

}  // namespace lifecycle
