// Host shadow of HalDisplay — just the display dimensions DirectPixelWriter
// references (the image render path is never executed in the sim).
#pragma once
#include <cstdint>
class HalDisplay {
 public:
  static constexpr uint16_t DISPLAY_WIDTH = 600;
  static constexpr uint16_t DISPLAY_HEIGHT = 800;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;
};
