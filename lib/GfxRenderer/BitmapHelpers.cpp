#include "BitmapHelpers.h"

#include <cstdint>

// Mirrors SETTINGS.useFactoryLUT. Updated from src/ via setBitmapHelpersUseFactoryLUT()
// at boot and on settings changes. Keeps BitmapHelpers a leaf module (no src/ include).
static bool g_useFactoryLUT = false;

void setBitmapHelpersUseFactoryLUT(bool enabled) { g_useFactoryLUT = enabled; }
bool bitmapHelpersUseFactoryLUT() { return g_useFactoryLUT; }

// Brightness/Contrast adjustments:
constexpr bool USE_BRIGHTNESS = true;        // Apply contrast adjustment before dithering
constexpr int BRIGHTNESS_BOOST = 0;          // No brightness offset (contrast alone is sufficient)
constexpr bool GAMMA_CORRECTION = false;     // Gamma curve (brightens midtones)
constexpr float CONTRAST_FACTOR = 1.15f;     // Contrast multiplier (1.0 = no change, >1 = more contrast)
constexpr bool USE_NOISE_DITHERING = false;  // Hash-based noise dithering

// Integer approximation of gamma correction (brightens midtones)
// Uses a simple curve: out = 255 * sqrt(in/255) ≈ sqrt(in * 255)
static inline int applyGamma(int gray) {
  if (!GAMMA_CORRECTION) return gray;
  // Fast integer square root approximation for gamma ~0.5 (brightening)
  // This brightens dark/mid tones while preserving highlights
  const int product = gray * 255;
  // Newton-Raphson integer sqrt (2 iterations for good accuracy)
  int x = gray;
  if (x > 0) {
    x = (x + product / x) >> 1;
    x = (x + product / x) >> 1;
  }
  return x > 255 ? 255 : x;
}

// Apply contrast adjustment around midpoint (128)
// factor > 1.0 increases contrast, < 1.0 decreases
static inline int applyContrast(int gray) {
  // Integer-based contrast: (gray - 128) * factor + 128
  // Using fixed-point: factor 1.15 ≈ 115/100
  constexpr int factorNum = static_cast<int>(CONTRAST_FACTOR * 100);
  int adjusted = ((gray - 128) * factorNum) / 100 + 128;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;
  return adjusted;
}
// Combined brightness/contrast/gamma adjustment.
// Factory LUT mode: returns input unchanged. Cover BMPs would otherwise be
// double-brightened (once at generation, once at render) under the factory
// waveform's already-soft drive; both passes are skipped to match upstream's
// blown-out-cover fix.
int adjustPixel(int gray) {
  if (bitmapHelpersUseFactoryLUT()) return gray;
  if (!USE_BRIGHTNESS) return gray;

  // Order: contrast first, then brightness, then gamma
  gray = applyContrast(gray);
  gray += BRIGHTNESS_BOOST;
  if (gray > 255) gray = 255;
  if (gray < 0) gray = 0;
  gray = applyGamma(gray);

  return gray;
}
// Simple quantization without dithering - divide into 4 levels
// Thresholds match the Atkinson/Floyd-Steinberg ditherers (see header).
uint8_t quantizeSimple(int gray) {
  if (bitmapHelpersUseFactoryLUT()) {
    if (gray < 43) {
      return 0;
    } else if (gray < 128) {
      return 1;
    } else if (gray < 213) {
      return 2;
    } else {
      return 3;
    }
  }
  if (gray < 40) {
    return 0;
  } else if (gray < 105) {
    return 1;
  } else if (gray < 180) {
    return 2;
  } else {
    return 3;
  }
}

// Hash-based noise dithering - survives downsampling without moiré artifacts
// Uses integer hash to generate pseudo-random threshold per pixel
static inline uint8_t quantizeNoise(int gray, int x, int y) {
  uint32_t hash = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
  hash = (hash ^ (hash >> 13)) * 1274126177u;
  const int threshold = static_cast<int>(hash >> 24);

  const int scaled = gray * 3;
  if (scaled < 255) {
    return (scaled + threshold >= 255) ? 1 : 0;
  } else if (scaled < 510) {
    return ((scaled - 255) + threshold >= 255) ? 2 : 1;
  } else {
    return ((scaled - 510) + threshold >= 255) ? 3 : 2;
  }
}

// Main quantization function - selects between methods based on config
uint8_t quantize(int gray, int x, int y) {
  if (USE_NOISE_DITHERING) {
    return quantizeNoise(gray, x, y);
  } else {
    return quantizeSimple(gray);
  }
}

// 1-bit noise dithering for fast home screen rendering
// Uses hash-based noise for consistent dithering that works well at small sizes
uint8_t quantize1bit(int gray, int x, int y) {
  gray = adjustPixel(gray);

  // Generate noise threshold using integer hash (no regular pattern to alias)
  uint32_t hash = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
  hash = (hash ^ (hash >> 13)) * 1274126177u;
  const int threshold = static_cast<int>(hash >> 24);  // 0-255

  // Simple threshold with noise: gray >= (128 + noise offset) -> white
  // The noise adds variation around the 128 midpoint
  const int adjustedThreshold = 128 + ((threshold - 128) / 2);  // Range: 64-192
  return (gray >= adjustedThreshold) ? 1 : 0;
}
