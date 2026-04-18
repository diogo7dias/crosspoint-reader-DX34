#pragma once

#include <cstdint>
#include <string>

namespace lifecycle {

enum class RouteId : uint8_t {
  Home,
  Reader,  // payload: path
  MyLibrary,
  MyLibraryAt,  // payload: path
  RecentBooks,
  Settings,
  FileTransfer,
  Browser,
};

struct Nav {
  RouteId route;
  std::string payload;
};

// Per-route side-effect policy applied by ActivityRouter before invoking the
// route factory. Collapses the 9 inline persistAppState call sites into a
// single exhaustive switch (compile error if a new RouteId misses a row).
struct RoutePolicy {
  bool persistBefore = false;
  bool trimSleepFolder = false;
  const char* persistCtx = nullptr;
};

constexpr RoutePolicy policyFor(RouteId r) {
  switch (r) {
    case RouteId::Home:
      return {true, true, "go home"};
    case RouteId::Reader:
      return {false, false, nullptr};
    case RouteId::MyLibrary:
      return {true, false, "go to library"};
    case RouteId::MyLibraryAt:
      return {true, false, "go to library path"};
    case RouteId::RecentBooks:
      return {true, false, "go to recents"};
    case RouteId::Settings:
      return {false, false, nullptr};
    case RouteId::FileTransfer:
      return {false, false, nullptr};
    case RouteId::Browser:
      return {false, false, nullptr};
  }
  return {};  // unreachable; satisfies non-void return warning
}

}  // namespace lifecycle
