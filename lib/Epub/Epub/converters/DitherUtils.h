#pragma once

#include <stdint.h>
#include <string.h>

// 4x4 Bayer matrix for ordered dithering
inline const uint8_t bayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

// Apply Bayer dithering and quantize to 4 levels (0-3)
// Stateless - works correctly with any pixel processing order.
// A non-linear brightness boost lifts dark/mid-tones for better e-ink
// photograph rendering while barely affecting highlights.
inline uint8_t applyBayerDither4Level(uint8_t gray, int x, int y) {
  int boost = (255 - gray) / 6;
  int boosted = gray + boost;

  int bayer = bayer4x4[y & 3][x & 3];
  int dither = (bayer - 8) * 5;  // Scale to +/-40 (half of quantization step 85)

  int adjusted = boosted + dither;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;

  if (adjusted < 64) return 0;
  if (adjusted < 128) return 1;
  if (adjusted < 192) return 2;
  return 3;
}

// Floyd-Steinberg error-diffusion ditherer for 4-level quantization.
// Produces dramatically better photograph quality than Bayer by spreading
// quantization error to neighboring pixels, creating the illusion of many
// more gray levels. Requires row-sequential, left-to-right processing.
//
// Memory: two rows of int16_t error (~3.2 KB for 800px width).
// Call init() once per image, then beginRow()/dither4Level()/finishRow() per row.
class EpubFloydSteinbergDitherer {
 public:
  void init(int width) {
    width_ = width;
    // Two rows of error: current (being consumed) and next (being accumulated)
    // +2 for boundary padding (left/right neighbors at edges)
    const int padded = width + 2;
    if (padded > MAX_WIDTH + 2) return;  // Safety: refuse absurdly wide images
    memset(errCurr_, 0, padded * sizeof(int16_t));
    memset(errNext_, 0, padded * sizeof(int16_t));
  }

  // Call at the start of each destination row
  void beginRow() {
    // Swap current/next: promote accumulated next-row error to current
    int16_t* tmp = errCurr_;
    errCurr_ = errNext_;
    errNext_ = tmp;
    memset(errNext_, 0, (width_ + 2) * sizeof(int16_t));
  }

  // Dither one pixel. Must be called left-to-right for x = 0..width-1.
  // Returns 4-level quantized value (0-3).
  inline uint8_t dither4Level(uint8_t gray, int x) {
    // Apply same non-linear brightness boost as Bayer for e-ink consistency
    int boost = (255 - gray) / 6;
    int val = gray + boost;

    // Add accumulated error from previous pixels (index offset +1 for padding)
    val += errCurr_[x + 1];

    // Clamp
    if (val < 0) val = 0;
    if (val > 255) val = 255;

    // Quantize to 4 levels using same thresholds as Bayer
    uint8_t quantized;
    int target;
    if (val < 64) {
      quantized = 0;
      target = 0;
    } else if (val < 128) {
      quantized = 1;
      target = 85;
    } else if (val < 192) {
      quantized = 2;
      target = 170;
    } else {
      quantized = 3;
      target = 255;
    }

    // Compute quantization error and diffuse to neighbors
    int err = val - target;
    // Floyd-Steinberg distribution: right 7/16, below-left 3/16, below 5/16, below-right 1/16
    errCurr_[x + 2] += (err * 7) >> 4;  // right neighbor (current row)
    errNext_[x] += (err * 3) >> 4;      // below-left (next row)
    errNext_[x + 1] += (err * 5) >> 4;  // below (next row)
    errNext_[x + 2] += (err * 1) >> 4;  // below-right (next row)

    return quantized;
  }

 private:
  static constexpr int MAX_WIDTH = 1024;  // Max supported image width
  int width_ = 0;
  // +2 padding on each side for boundary neighbors
  int16_t errBuf0_[MAX_WIDTH + 2] = {};
  int16_t errBuf1_[MAX_WIDTH + 2] = {};
  int16_t* errCurr_ = errBuf0_;
  int16_t* errNext_ = errBuf1_;
};
