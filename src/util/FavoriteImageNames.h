#pragma once

// Pure suffix helpers for sleep-image filenames.
// No Arduino types, no APP_STATE, no Storage — safe for host-side testing.
// FavoriteImage.h re-exports these; FavoriteImage.cpp includes this header.

#include <string>

namespace FavoriteImage {

// Returns true if `filename` has a recognized favorite-eligible image
// extension (.bmp or .pxc, case-insensitive). Single source of truth for
// the supported-extension list; FavoriteImage.cpp::isImagePath delegates
// here.
bool isImageExtension(const std::string& filename);

bool hasFavoriteSuffix(const std::string& filename);
std::string stripFavoriteSuffix(const std::string& filename);
std::string addFavoriteSuffix(const std::string& filename);

}  // namespace FavoriteImage
