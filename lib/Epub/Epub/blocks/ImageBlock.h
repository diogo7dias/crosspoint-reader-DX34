#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

class ImageBlock final : public Block {
public:
  ImageBlock(const std::string &imagePath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string &getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  bool imageExists() const;

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer &renderer, const int x, const int y);
  bool serialize(FsFile &file);
  static std::unique_ptr<ImageBlock> deserialize(FsFile &file);

  // Set global dither mode for all image rendering (0=Bayer, 1=Floyd-Steinberg)
  static void setDitherMode(uint8_t mode) { ditherMode_ = mode; }
  static uint8_t getDitherMode() { return ditherMode_; }

private:
  static uint8_t ditherMode_;
  std::string imagePath;
  int16_t width;
  int16_t height;
};
