#include "FavoriteImage.h"

#include <HalStorage.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "CrossPointState.h"
#include "util/FavoriteImageNames.h"

namespace FavoriteImage {
namespace {

bool isImagePath(const std::string& path) {
  return FavoriteImage::isImageExtension(path);
}

bool startsWith(const std::string& value, const char* prefix) { return value.rfind(prefix, 0) == 0; }

std::string getBasename(const std::string& path) {
  const auto slashPos = path.find_last_of('/');
  return (slashPos == std::string::npos) ? path : path.substr(slashPos + 1);
}

std::string getParentPath(const std::string& path) {
  const auto slashPos = path.find_last_of('/');
  if (slashPos == std::string::npos || slashPos == 0) {
    return "/";
  }
  return path.substr(0, slashPos);
}

std::string joinPath(const std::string& parent, const std::string& name) {
  if (parent.empty() || parent == "/") {
    return "/" + name;
  }
  return parent + "/" + name;
}

bool vectorContains(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

void addUnique(std::vector<std::string>& values, const std::string& value) {
  if (!vectorContains(values, value)) {
    values.push_back(value);
  }
}

void removeValue(std::vector<std::string>& values, const std::string& value) {
  values.erase(std::remove(values.begin(), values.end(), value), values.end());
}

bool isInSleepFolder(const std::string& path) { return startsWith(path, "/sleep/"); }

void updateSleepReferencesOnPathChange(const std::string& oldPath, const std::string& newPath) {
  const bool oldInSleep = isInSleepFolder(oldPath);
  const bool newInSleep = isInSleepFolder(newPath);
  const std::string oldBase = getBasename(oldPath);
  const std::string newBase = getBasename(newPath);

  if (oldInSleep && newInSleep) {
    for (std::string& entry : APP_STATE.sleepImagePlaylist) {
      if (entry == oldBase) {
        entry = newBase;
      }
    }
    if (APP_STATE.lastShownSleepFilename == oldBase) {
      APP_STATE.lastShownSleepFilename = newBase;
    }
  } else if (oldInSleep && !newInSleep) {
    removeValue(APP_STATE.sleepImagePlaylist, oldBase);
    if (APP_STATE.lastShownSleepFilename == oldBase) {
      APP_STATE.lastShownSleepFilename.clear();
    }
  }

  if (APP_STATE.lastSleepWallpaperPath == oldPath) {
    APP_STATE.lastSleepWallpaperPath = newPath;
  }
}

void removeSleepReferencesForPath(const std::string& path) {
  if (APP_STATE.lastSleepWallpaperPath == path) {
    APP_STATE.lastSleepWallpaperPath.clear();
  }

  if (isInSleepFolder(path)) {
    const std::string base = getBasename(path);
    removeValue(APP_STATE.sleepImagePlaylist, base);
    if (APP_STATE.lastShownSleepFilename == base) {
      APP_STATE.lastShownSleepFilename.clear();
    }
  }
}

}  // namespace

// hasFavoriteSuffix, addFavoriteSuffix, stripFavoriteSuffix are defined in
// FavoriteImageNames.cpp (pure helpers, no Arduino/APP_STATE deps).

bool isFavoritePath(const std::string& path) {
  return vectorContains(APP_STATE.favoriteBmpPaths, path) || hasFavoriteSuffix(getBasename(path));
}

bool canPlacePathInSleep(const std::string& path) {
  if (!isImagePath(path) || !isFavoritePath(path) || isInSleepFolder(path)) {
    return true;
  }
  return countProtectedSleepFavorites() < CrossPointState::SLEEP_FAVORITES_MAX;
}

bool isSleepFavoritesFull() { return countProtectedSleepFavorites() >= CrossPointState::SLEEP_FAVORITES_MAX; }

size_t countProtectedSleepFavorites() {
  size_t count = 0;
  auto dir = Storage.open("/sleep");
  if (!dir || !dir.isDirectory()) {
    if (dir) {
      dir.close();
    }
    return 0;
  }

  char name[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) {
      file.close();
      continue;
    }

    file.getName(name, sizeof(name));
    std::string filename(name);
    if (!filename.empty() && filename[0] != '.' && isImagePath(filename) &&
        isFavoritePath("/sleep/" + filename)) {
      ++count;
    }
    file.close();
  }
  dir.close();
  return count;
}

std::string displayNameForPath(const std::string& path) {
  const std::string filename = getBasename(path);
  if (!isFavoritePath(path)) {
    return filename;
  }
  return std::string("[F] ") + stripFavoriteSuffix(filename);
}

const char* limitReachedPopupMessage() {
  static char buf[48];
  static bool initialized = false;
  if (!initialized) {
    snprintf(buf, sizeof(buf), "Sleep favorites full (%zu/%zu)", CrossPointState::SLEEP_FAVORITES_MAX,
             CrossPointState::SLEEP_FAVORITES_MAX);
    initialized = true;
  }
  return buf;
}

const char* limitReachedHomeMessage() {
  static char buf[48];
  static bool initialized = false;
  if (!initialized) {
    snprintf(buf, sizeof(buf), "Sleep favorites full: %zu/%zu", CrossPointState::SLEEP_FAVORITES_MAX,
             CrossPointState::SLEEP_FAVORITES_MAX);
    initialized = true;
  }
  return buf;
}

SetFavoriteResult setFavorite(const std::string& path, const bool favorite, std::string* updatedPath) {
  if (!isImagePath(path)) {
    return SetFavoriteResult::NotImage;
  }
  if (!Storage.exists(path.c_str())) {
    return SetFavoriteResult::Missing;
  }

  std::string currentPath = path;
  const bool alreadyFavorite = isFavoritePath(currentPath);
  if (favorite && isInSleepFolder(currentPath) && !alreadyFavorite && isSleepFavoritesFull()) {
    return SetFavoriteResult::LimitReached;
  }

  const std::string currentName = getBasename(currentPath);
  const std::string targetName = favorite ? addFavoriteSuffix(currentName) : stripFavoriteSuffix(currentName);

  if (targetName != currentName) {
    const std::string targetPath = joinPath(getParentPath(currentPath), targetName);
    if (targetPath != currentPath) {
      if (Storage.exists(targetPath.c_str())) {
        return SetFavoriteResult::RenameConflict;
      }
      if (!Storage.rename(currentPath.c_str(), targetPath.c_str())) {
        return SetFavoriteResult::RenameFailed;
      }
      replacePathReferences(currentPath, targetPath);
      currentPath = targetPath;
    }
  }

  if (favorite) {
    addUnique(APP_STATE.favoriteBmpPaths, currentPath);
  } else {
    removeValue(APP_STATE.favoriteBmpPaths, currentPath);
  }

  APP_STATE.saveToFile();
  if (updatedPath != nullptr) {
    *updatedPath = currentPath;
  }
  return SetFavoriteResult::Success;
}

void replacePathReferences(const std::string& oldPath, const std::string& newPath) {
  if (oldPath == newPath) {
    return;
  }

  for (std::string& path : APP_STATE.favoriteBmpPaths) {
    if (path == oldPath) {
      path = newPath;
    }
  }

  updateSleepReferencesOnPathChange(oldPath, newPath);
}

void removePathReferences(const std::string& path) {
  removeValue(APP_STATE.favoriteBmpPaths, path);
  removeSleepReferencesForPath(path);
}

}  // namespace FavoriteImage
