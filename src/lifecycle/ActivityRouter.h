#pragma once

#include <functional>
#include <optional>
#include <string>

#include "Nav.h"

class Activity;

namespace lifecycle {

// ActivityRouter — scaffold only (issue #23).
// Owns activity transitions, persist coalescing, and deep-sleep entry once
// call sites are migrated. Today all methods are no-ops; main.cpp is untouched.
// Migration is gated per call site in follow-up PRs.
class ActivityRouter {
 public:
  static ActivityRouter& instance();

  void begin(const Nav& initial);
  void request(const Nav& nav);
  void applyIfPending();
  void enterDeepSleep(bool fromReader);

  // main.cpp owns activity construction (it holds the globals: renderer,
  // mappedInputManager, onGoHome, etc.). It registers one factory per route;
  // applyIfPending dispatches on the pending Nav and calls the factory.
  using Factory = std::function<void(const std::string& payload)>;
  void setRouteFactory(RouteId route, Factory factory);

  std::function<void(const std::string&)> makeGoToReader() const;
  std::function<void()>                   makeGoHome() const;
  std::function<void()>                   makeGoToSettings() const;
  std::function<void()>                   makeGoToMyLibrary() const;
  std::function<void(const std::string&)> makeGoToMyLibraryWithPath() const;
  std::function<void()>                   makeGoToRecentBooks() const;
  std::function<void()>                   makeGoToFileTransfer() const;
  std::function<void()>                   makeGoToBrowser() const;

  struct Deps {
    Activity** currentActivitySlot = nullptr;
    bool (*persistAppState)(const char* ctx) = nullptr;
    void (*onBeforeDeepSleep)() = nullptr;
    void (*onAfterDeepSleep)() = nullptr;
  };
  static void setDepsForTest(Deps deps);

 private:
  ActivityRouter() = default;
  static constexpr size_t kRouteCount = 8;
  std::optional<Nav> pending_;
  bool busy_ = false;
  Deps deps_{};
  Factory factories_[kRouteCount];
};

}  // namespace lifecycle
