/**
 * @file SleepActivity.h
 * @brief Sleep/screensaver screen — displays BMP images before deep sleep.
 *
 * Manages a playlist of BMP images from the /sleep folder on the SD card.
 * Small collections (<=200 images) use a full shuffle playlist persisted in
 * state.json. Large collections use sequential advance with wrap-around.
 * After rendering the image, the device enters RTC deep sleep.
 */
#pragma once
#include "../Activity.h"

class Bitmap;
class GfxRenderer;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Sleep", renderer, mappedInput) {}
  void onEnter() override;
  static bool randomizeSleepImagePlaylist();
  // Called once on boot: moves overflow images beyond the playlist limit to /sleep pause.
  // Also caches the number of protected sleep favorites for fast access.
  static void trimSleepFolderToLimit(GfxRenderer* renderer = nullptr);
  // Return the favorite count cached by the last trimSleepFolderToLimit() call.
  static size_t cachedSleepFavoriteCount();

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap, const char* sourceFilename = nullptr) const;
  void renderBlankSleepScreen() const;
  void renderQuotesSleepScreen() const;
};
