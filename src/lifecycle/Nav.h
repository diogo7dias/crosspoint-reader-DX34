#pragma once

#include <cstdint>
#include <string>

namespace lifecycle {

enum class RouteId : uint8_t {
  Home,
  Reader,        // payload: path
  MyLibrary,
  MyLibraryAt,   // payload: path
  RecentBooks,
  Settings,
  FileTransfer,
  Browser,
};

struct Nav {
  RouteId route;
  std::string payload;
};

}  // namespace lifecycle
