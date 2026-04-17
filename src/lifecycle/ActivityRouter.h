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
  std::optional<Nav> pending_;
  bool busy_ = false;
  Deps deps_{};
};

}  // namespace lifecycle
