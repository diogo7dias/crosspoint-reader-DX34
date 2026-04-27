#include "LibraryListingFilter.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace LibraryListingFilter {

void filterEpubCachePxc(std::vector<std::string>& files) {
  // Source extensions whose presence protects a same-stem .pxc from display.
  // .bmp is intentionally excluded — a .bmp + .pxc pair in /sleep are two
  // independent user wallpapers, not a source/cache pair.
  static const char* const kSourceExts[] = {".jpg", ".jpeg", ".png", ".gif", ".webp"};

  auto lowerExt = [](const std::string& name, size_t extLen) {
    if (name.size() < extLen) return std::string{};
    std::string ext = name.substr(name.size() - extLen);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
  };

  auto stemOf = [](const std::string& name) {
    const auto dot = name.find_last_of('.');
    return (dot == std::string::npos) ? name : name.substr(0, dot);
  };

  auto toLower = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
  };

  // Build set of lowercased stems for which a source-format sibling exists.
  std::unordered_set<std::string> sourceStems;
  for (const auto& name : files) {
    if (name.empty() || name.back() == '/') continue;
    for (const char* ext : kSourceExts) {
      const std::string e = ext;
      if (name.size() >= e.size() && lowerExt(name, e.size()) == e) {
        sourceStems.insert(toLower(stemOf(name)));
        break;
      }
    }
  }

  files.erase(std::remove_if(files.begin(), files.end(),
                             [&](const std::string& name) {
                               if (name.empty() || name.back() == '/') return false;
                               if (lowerExt(name, 4) != ".pxc") return false;
                               const std::string lower = toLower(name);
                               // Drop any *_q.pxc.
                               if (lower.size() >= 6 && lower.compare(lower.size() - 6, 6, "_q.pxc") == 0) {
                                 return true;
                               }
                               // Drop *.pxc whose stem has a source-format sibling.
                               return sourceStems.count(toLower(stemOf(name))) > 0;
                             }),
              files.end());
}

}  // namespace LibraryListingFilter
