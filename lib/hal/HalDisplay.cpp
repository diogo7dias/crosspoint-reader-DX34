#include <HalDisplay.h>
#include <HalGPIO.h>

#define SD_SPI_MISO 7

// Global HalGPIO instance (defined in src/main.cpp) for the device-type gate.
extern HalGPIO gpio;

HalDisplay::HalDisplay() : einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY) {}

HalDisplay::~HalDisplay() {}

void HalDisplay::begin() {
  // On the X3, flip the SDK driver to the 792x528 panel geometry + waveforms
  // BEFORE init. This makes the runtime getters report X3 dimensions. Never
  // called on an X4, so the X4 panel path is unchanged.
  // NOTE: upstream also forces a RED-RAM resync on HALF refreshes via a
  // displayGrayscaleBase() path to kill X3 cover-transition ghosting. Our
  // HalDisplay wraps displayGrayBuffer() and has no displayGrayscaleBase seam,
  // so that ghosting fix is deferred until/if we adopt that grayscale path.
  if (gpio.deviceIsX3()) {
    einkDisplay.setDisplayX3();
  }
  einkDisplay.begin();
}

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }
uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }
uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }
uint32_t HalDisplay::getBufferSize() const { return einkDisplay.getBufferSize(); }

void HalDisplay::clearScreen(uint8_t color) const { einkDisplay.clearScreen(color); }

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  einkDisplay.drawImage(imageData, x, y, w, h, fromProgmem);
}

EInkDisplay::RefreshMode convertRefreshMode(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return EInkDisplay::FULL_REFRESH;
    case HalDisplay::HALF_REFRESH:
      return EInkDisplay::HALF_REFRESH;
    case HalDisplay::FAST_REFRESH:
    default:
      return EInkDisplay::FAST_REFRESH;
  }
}

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  einkDisplay.refreshDisplay(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::deepSleep() { einkDisplay.deepSleep(); }

uint8_t* HalDisplay::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  einkDisplay.copyGrayscaleBuffers(lsbBuffer, msbBuffer);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer); }

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { einkDisplay.copyGrayscaleMsbBuffers(msbBuffer); }

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { einkDisplay.cleanupGrayscaleBuffers(bwBuffer); }

void HalDisplay::displayGrayBuffer(bool turnOffScreen, const uint8_t* lut, bool factoryMode) {
  einkDisplay.displayGrayBuffer(turnOffScreen, lut, factoryMode);
}
