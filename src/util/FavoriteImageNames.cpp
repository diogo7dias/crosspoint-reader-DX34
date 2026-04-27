// Pure suffix helpers for sleep-image filenames (.bmp / .pxc).
// Deliberately free of Arduino types, APP_STATE, and Storage so this
// translation unit can be compiled in the host test environment.

#include "FavoriteImageNames.h"

#include <cctype>
#include <cstring>
#include <string>

namespace FavoriteImage {
namespace {

constexpr const char* kFavoriteSuffix = "_F";

bool isImageExtension(const std::string& filename) {
  if (filename.size() < 4) return false;
  const std::string ext = filename.substr(filename.size() - 4);
  // Case-insensitive compare against .bmp and .pxc
  auto lc = [](const std::string& s) {
    std::string r = s;
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
  };
  const std::string extLc = lc(ext);
  return extLc == ".bmp" || extLc == ".pxc";
}

}  // namespace

bool hasFavoriteSuffix(const std::string& filename) {
  if (!isImageExtension(filename) || filename.size() <= 6) {
    return false;
  }
  const size_t extPos = filename.size() - 4;
  return filename.substr(extPos - 2, 2) == "_F";
}

std::string addFavoriteSuffix(const std::string& filename) {
  if (!isImageExtension(filename) || hasFavoriteSuffix(filename)) {
    return filename;
  }
  return filename.substr(0, filename.size() - 4) + kFavoriteSuffix + filename.substr(filename.size() - 4);
}

std::string stripFavoriteSuffix(const std::string& filename) {
  if (!hasFavoriteSuffix(filename)) {
    return filename;
  }
  const size_t extPos = filename.size() - 4;
  return filename.substr(0, extPos - 2) + filename.substr(extPos);
}

}  // namespace FavoriteImage
