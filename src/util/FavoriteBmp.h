#pragma once

#include <cstddef>
#include <string>

namespace FavoriteBmp {

enum class SetFavoriteResult {
  Success,
  NotBmp,
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

}  // namespace FavoriteBmp
