#pragma once

#include <EpdFontFamily.h>
#include <HalDisplay.h>

class FontCacheManager;

#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "Bitmap.h"

// Color representation: uint8_t mapped to 4x4 Bayer matrix dithering levels
// 0 = transparent, 1-16 = gray levels (white to black)
enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

class GfxRenderer {
 public:
  enum RenderMode {
    BW,             // 1-bit: clearScreen(0xFF) base; drawPixel(true) clears bit (black on white)
    GRAYSCALE_LSB,  // Differential gray LSB plane: clearScreen(0x00) base; drawPixel(false) sets bit
    GRAYSCALE_MSB,  // Differential gray MSB plane: clearScreen(0x00) base; drawPixel(false) sets bit
    GRAY2_LSB,      // Factory absolute gray BW RAM: clearScreen(0x00) base; drawPixel(false) sets bit
    GRAY2_MSB,      // Factory absolute gray RED RAM: clearScreen(0x00) base; drawPixel(false) sets bit
  };

  // Selects LUT and behavior for renderGrayscale().
  enum class GrayscaleMode {
    FactoryFast,     // Factory absolute 2-bit (lut_factory_fast); HALF_REFRESH pre-flash
    FactoryQuality,  // Factory absolute 2-bit (lut_factory_quality); HALF_REFRESH pre-flash
    Differential,    // Differential 2-bit overlay (lut_grayscale); no pre-flash, requires prior BW state
  };

  // Logical screen orientation from the perspective of callers
  enum Orientation {
    Portrait,                  // 480x800 logical coordinates (current default)
    LandscapeClockwise,        // 800x480 logical coordinates, rotated 180° (swap
                               // top/bottom)
    PortraitInverted,          // 480x800 logical coordinates, inverted
    LandscapeCounterClockwise  // 800x480 logical coordinates, native panel
                               // orientation
  };

 private:
  static constexpr size_t BW_BUFFER_CHUNK_SIZE = 8000;  // 8KB chunks to allow for non-contiguous memory

  // Runtime panel geometry, cached in begin() from the display driver. Defaults
  // are the X4 panel; on the X3 they become 792x528 after setDisplayX3(). All
  // render math reads these members so a single binary renders correctly on both
  // panels. The chunk store below is sized from frameBufferSize at begin().
  uint16_t panelWidth = HalDisplay::DISPLAY_WIDTH;
  uint16_t panelHeight = HalDisplay::DISPLAY_HEIGHT;
  uint16_t panelWidthBytes = HalDisplay::DISPLAY_WIDTH_BYTES;
  uint32_t frameBufferSize = HalDisplay::BUFFER_SIZE;

  HalDisplay& display;
  RenderMode renderMode;
  Orientation orientation;
  bool fadingFix;
  bool darkModeInvert;
  bool pendingFullRefresh;
  bool pendingHalfRefresh;
  uint8_t textRenderStyle;  // weight order: 0=thin 1=crisp 2=medium 3=dark 4=bionic
  uint8_t* frameBuffer = nullptr;
  // Chunked copy of the BW framebuffer during a grayscale render. Sized in
  // begin() to ceil(frameBufferSize / BW_BUFFER_CHUNK_SIZE): X4 48000 -> 6
  // chunks, X3 52272 -> 7 chunks (last chunk partial, clamped on copy).
  std::vector<uint8_t*> bwBufferChunks;
  // Built-in fonts and user-installed .bin (CPBN) fonts share this map:
  // the CPBN loader hands an EpdFontFamily backed by SD-resident data to
  // insertFont() just like the built-ins do, so the text-dispatch paths
  // below stay on a single fast path.
  std::map<int, EpdFontFamily> fontMap;

  // Mutable because drawText() is const but needs to delegate scan-mode
  // recording to the (non-const) FontCacheManager.
  mutable FontCacheManager* fontCacheManager_ = nullptr;

  void renderChar(const EpdFontFamily& fontFamily, uint32_t cp, int* x, int* y, bool pixelState,
                  EpdFontFamily::Style style) const;
  void freeBwBufferChunks();
  template <Color color>
  void drawPixelDither(int x, int y) const;
  template <Color color>
  void fillArc(int maxRadius, int cx, int cy, int xDir, int yDir) const;

 public:
  explicit GfxRenderer(HalDisplay& halDisplay)
      : display(halDisplay),
        renderMode(BW),
        orientation(Portrait),
        fadingFix(false),
        darkModeInvert(false),
        pendingFullRefresh(false),
        pendingHalfRefresh(false),
        textRenderStyle(1) {}  // 1 = crisp (weight-order default)
  ~GfxRenderer();

  static constexpr int VIEWABLE_MARGIN_TOP = 9;
  static constexpr int VIEWABLE_MARGIN_RIGHT = 3;
  static constexpr int VIEWABLE_MARGIN_BOTTOM = 3;
  static constexpr int VIEWABLE_MARGIN_LEFT = 3;

  // Setup
  void begin();  // must be called right after display.begin()
  void insertFont(int fontId, EpdFontFamily font);
  // Drops the font registered under `fontId`, if any. No-op when absent.
  void removeFont(int fontId);

  // Orientation control (affects logical width/height and coordinate
  // transforms)
  void setOrientation(const Orientation o) { orientation = o; }
  Orientation getOrientation() const { return orientation; }

  // Fading fix control
  void setFadingFix(const bool enabled) { fadingFix = enabled; }

  // Dark mode: invert framebuffer before sending to display
  void setDarkMode(const bool enabled) { darkModeInvert = enabled; }
  bool getDarkMode() const { return darkModeInvert; }

  // Request a full refresh on the next displayBuffer() call (e.g. on screen transitions)
  void requestFullRefresh() { pendingFullRefresh = true; }
  void requestHalfRefresh() { pendingHalfRefresh = true; }

  // Screen ops
  int getScreenWidth() const;
  int getScreenHeight() const;
  void displayBuffer(HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH);
  // EXPERIMENTAL: Windowed update - display only a rectangular region
  // void displayWindow(int x, int y, int width, int height) const;
  void invertScreen() const;
  void clearScreen(uint8_t color = 0xFF) const;
  void getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const;

  // Drawing
  void drawPixel(int x, int y, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, bool state) const;
  void drawArc(int maxRadius, int cx, int cy, int xDir, int yDir, int lineWidth, bool state) const;
  void drawRect(int x, int y, int width, int height, bool state = true) const;
  void drawRect(int x, int y, int width, int height, int lineWidth, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool roundTopLeft,
                       bool roundTopRight, bool roundBottomLeft, bool roundBottomRight, bool state) const;
  void fillRect(int x, int y, int width, int height, bool state = true) const;
  void fillRectDither(int x, int y, int width, int height, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, bool roundTopLeft, bool roundTopRight,
                       bool roundBottomLeft, bool roundBottomRight, Color color) const;
  void drawImage(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawIcon(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawBitmap(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight, float cropX = 0,
                  float cropY = 0) const;
  void drawBitmap1Bit(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight) const;
  void fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state = true) const;

  // Text
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextWidthSpaced(int fontId, const char* text, int letterSpacing,
                         EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void drawCenteredText(int fontId, int y, const char* text, bool black = true,
                        EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void drawText(int fontId, int x, int y, const char* text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void drawTextSpaced(int fontId, int x, int y, const char* text, int letterSpacing, bool black = true,
                      EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getSpaceWidth(int fontId, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextAdvanceXSpaced(int fontId, const char* text, int letterSpacing,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getFontAscenderSize(int fontId) const;
  int getLineHeight(int fontId) const;
  bool hasGlyph(int fontId, uint32_t cp, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  std::string truncatedText(int fontId, const char* text, int maxWidth,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;

  // Helper for drawing rotated text (90 degrees clockwise, for side buttons)
  void drawTextRotated90CW(int fontId, int x, int y, const char* text, bool black = true,
                           EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextHeight(int fontId) const;

  // Vertical ink extent of `text`, measured relative to the line-box top (the
  // `y` you would pass to drawText). On success fills inkTop/inkBottom with the
  // topmost/bottommost ink-pixel offsets (inkTop = ascender - tallestGlyph.top,
  // inkBottom = that + glyph.height). Whitespace-only / empty strings have no
  // ink and return false. Baseline uses the REGULAR ascender so the result
  // matches how drawText positions glyphs regardless of `style`.
  bool measureTextInk(int fontId, const char* text, int* inkTop, int* inkBottom,
                      EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;

  // Grayscale functions
  void setRenderMode(const RenderMode mode) { this->renderMode = mode; }
  RenderMode getRenderMode() const { return renderMode; }
  void setTextRenderStyle(const uint8_t style) { textRenderStyle = style; }
  uint8_t getTextRenderStyle() const { return textRenderStyle; }
  void copyGrayscaleLsbBuffers() const;
  void copyGrayscaleMsbBuffers() const;
  void displayGrayBuffer(const uint8_t* lut = nullptr, bool factoryMode = false) const;

  // Two-pass grayscale render. drawFn is called twice: once with the LSB render mode set
  // (writes BW RAM plane), then with the MSB mode set (writes RED RAM plane). The method
  // handles pre-flash (factory modes only), clearScreen, setRenderMode, buffer copies,
  // displayGrayBuffer, and resets renderMode to BW on completion. storeBwBuffer /
  // restoreBwBuffer remain the caller's responsibility.
  void renderGrayscale(GrayscaleMode mode, const std::function<void()>& drawFn);
  bool storeBwBuffer();    // Returns true if buffer was stored successfully
  void restoreBwBuffer();  // Restore and free the stored buffer
  void cleanupGrayscaleWithFrameBuffer() const;

  // Font cache manager
  void setFontCacheManager(FontCacheManager* m) { fontCacheManager_ = m; }
  FontCacheManager* getFontCacheManager() const { return fontCacheManager_; }
  // True during the font-cache prewarm pass, when the framebuffer is discarded.
  // Callers skip work whose only output is pixels (underline measure, image
  // decode) to avoid glyph-miss SD thrash / wasted decodes. Out-of-line: the
  // FontCacheManager type is only forward-declared here.
  bool isFontCacheScanning() const;
  const std::map<int, EpdFontFamily>& getFontMap() const { return fontMap; }

  // Font helpers
  const uint8_t* getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const;

  // Low level functions
  uint8_t* getFrameBuffer() const;
  size_t getBufferSize() const;
};
