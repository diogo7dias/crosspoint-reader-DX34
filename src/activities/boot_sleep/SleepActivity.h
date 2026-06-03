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

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap, const char* sourceFilename = nullptr) const;
  // Renders a .pxc (pre-dithered 2bpp) wallpaper via the factory grayscale
  // path. Returns true on success; false if the file can't be opened, the
  // header is unreadable, or the dimensions don't match the screen.
  bool renderPxcSleepScreen(const std::string& path, const char* sourceFilename = nullptr) const;
  void renderBlankSleepScreen() const;
  void renderQuotesSleepScreen() const;
  void renderFreezeSleepScreen() const;
};
