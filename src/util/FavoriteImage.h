#pragma once

#include <cstddef>
#include <string>

// Pure suffix helpers (hasFavoriteSuffix, addFavoriteSuffix, stripFavoriteSuffix)
// live in FavoriteImageNames.h / FavoriteImageNames.cpp so they can be compiled
// host-side for unit tests without pulling in Arduino / APP_STATE / Storage.
#include "FavoriteImageNames.h"

namespace FavoriteImage {

enum class SetFavoriteResult {
  Success,
  NotImage,
  Missing,
  LimitReached,
  RenameConflict,
  RenameFailed,
};

bool isFavoritePath(const std::string& path);
bool canPlacePathInSleep(const std::string& path);
bool isSleepFavoritesFull();
size_t countProtectedSleepFavorites();
std::string displayNameForPath(const std::string& path);
const char* limitReachedPopupMessage();
const char* limitReachedHomeMessage();
SetFavoriteResult setFavorite(const std::string& path, bool favorite, std::string* updatedPath = nullptr);
void replacePathReferences(const std::string& oldPath, const std::string& newPath);
void removePathReferences(const std::string& path);

}  // namespace FavoriteImage
