/**
 * @file SleepOrderPager.h
 * @brief Memory-safe paginator over the sleep-wallpaper rotation order.
 *
 * Backs the "View sleep wallpapers" settings screen. The rotation order lives
 * in /.crosspoint/sleep_order.txt as a "v1 cursor=N" header followed by one
 * basename per line (see WallpaperPlaylistV2). Reading the whole file into a
 * std::string (IFileIO::safeRead) is exactly the contiguous-allocation that
 * trips bad_alloc on a fragmented sleep-entry heap, so the viewer must NEVER
 * do that.
 *
 * Instead this walks the order source one line at a time via an injected
 * OrderLineReader and materializes only the names on the requested page. Peak
 * heap is bounded by the page window (~12 strings), independent of folder size.
 * The reader abstraction keeps the logic pure and host-testable: production
 * wraps a streamed HalFile, host tests feed a std::vector.
 */
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace crosspoint {
namespace sleep {

struct OrderPage {
  std::vector<std::string> names;  // entries whose index falls in [start, start+count)
  size_t total = 0;                // total wallpaper entries (header excluded)
};

// Yields the next raw line (without trailing newline) into `out`. Returns true
// if a line was produced, false at end of input. Mirrors an SD file streamed
// with a manual read()/'\n' loop.
using OrderLineReader = std::function<bool(std::string& out)>;

// Single streaming pass over the order source. Skips a leading "v1 cursor=..."
// header line when present, ignores blank lines, counts every entry into
// `total`, and collects the names whose entry index is in [start, start+count)
// into `names`. O(1) heap beyond the returned page window.
inline OrderPage readSleepOrderPage(const OrderLineReader& nextLine, size_t start, size_t count) {
  OrderPage page;
  std::string line;
  bool first = true;
  size_t idx = 0;
  while (nextLine(line)) {
    if (first) {
      first = false;
      // The order file always opens with a "v1 cursor=N" header; drop it so it
      // is neither counted nor shown. A source without the header (legacy or
      // the folder-scan fallback) treats its first line as a normal entry.
      if (line.size() >= 3 && line.compare(0, 3, "v1 ") == 0 && line.find("cursor=") != std::string::npos) {
        continue;
      }
    }
    if (line.empty()) continue;  // tolerate blank / trailing newline lines
    const size_t i = idx++;
    if (i >= start && i < start + count) {
      page.names.push_back(line);
    }
  }
  page.total = idx;
  return page;
}

}  // namespace sleep
}  // namespace crosspoint
