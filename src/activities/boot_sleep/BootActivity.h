#pragma once
#include "../Activity.h"

class BootActivity final : public Activity {
 public:
  explicit BootActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Boot", renderer, mappedInput) {}
  void onEnter() override;
  void setProgress(int percent, const char* status = nullptr);

 private:
  bool tryDrawCustomBootImage() const;
  void drawStaticBootScreen() const;
  void drawDynamicBootScreen() const;
  void renderEmbeddedBootScreen(HalDisplay::RefreshMode refreshMode, bool fullRedraw) const;

  int progressPercent = 12;
  mutable bool customBootImageLoaded = false;
};
