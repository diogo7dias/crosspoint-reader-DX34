#pragma once

#include <cstddef>
#include <string>

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
bool hasFavoriteSuffix(const std::string& filename);
std::string stripFavoriteSuffix(const std::string& filename);
std::string addFavoriteSuffix(const std::string& filename);
const char* limitReachedPopupMessage();
const char* limitReachedHomeMessage();
SetFavoriteResult setFavorite(const std::string& path, bool favorite, std::string* updatedPath = nullptr);
void replacePathReferences(const std::string& oldPath, const std::string& newPath);
void removePathReferences(const std::string& path);

}  // namespace FavoriteImage
