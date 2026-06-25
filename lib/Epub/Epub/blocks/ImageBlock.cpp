#include "ImageBlock.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <new>

#include "../converters/DirectPixelWriter.h"
#include "../converters/ImageDecoderFactory.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

uint8_t ImageBlock::ditherMode_ = 0;

ImageBlock::ImageBlock(const std::string& imagePath, int16_t width, int16_t height)
    : imagePath(imagePath), width(width), height(height) {}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

namespace {

std::string getCachePath(const std::string& imagePath, uint8_t ditherMode) {
  // Replace extension with .pxc (pixel cache), suffixed by dither mode
  // so changing dither mode invalidates stale cache entries
  const char* suffix = (ditherMode == 1) ? "_q.pxc" : ".pxc";
  size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + suffix;
  }
  return imagePath + suffix;
}

bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight) {
  FsFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    cacheFile.close();
    return false;
  }

  // Verify dimensions are close (allow 1 pixel tolerance for rounding differences)
  int widthDiff = abs(cachedWidth - expectedWidth);
  int heightDiff = abs(cachedHeight - expectedHeight);
  if (widthDiff > 1 || heightDiff > 1) {
    LOG_ERR("IMG", "Cache dimension mismatch: %dx%d vs %dx%d", cachedWidth, cachedHeight, expectedWidth,
            expectedHeight);
    cacheFile.close();
    return false;
  }

  // Use cached dimensions for rendering (they're the actual decoded size)
  expectedWidth = cachedWidth;
  expectedHeight = cachedHeight;

  LOG_DBG("IMG", "Loading from cache: %s (%dx%d)", cachePath.c_str(), cachedWidth, cachedHeight);

  // Read and render row by row to minimize memory usage
  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
  auto rowBuffer = crosspoint::mem::CMallocPtr<uint8_t>(static_cast<uint8_t*>(crosspoint::mem::tryMalloc(bytesPerRow)));
  if (!rowBuffer) {
    LOG_ERR("IMG", "Failed to allocate row buffer");
    cacheFile.close();
    return false;
  }
  uint8_t* rb = rowBuffer.get();  // RAII owns; rb is a non-owning view for read/index

  DirectPixelWriter pw;
  pw.init(renderer);

  for (int row = 0; row < cachedHeight; row++) {
    if (cacheFile.read(rb, bytesPerRow) != bytesPerRow) {
      LOG_ERR("IMG", "Cache read error at row %d", row);
      cacheFile.close();
      return false;
    }

    const int destY = y + row;
    pw.beginRow(destY);
    for (int col = 0; col < cachedWidth; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      uint8_t pixelValue = (rb[byteIdx] >> bitShift) & 0x03;

      pw.writePixel(x + col, pixelValue);
    }
  }

  cacheFile.close();
  LOG_DBG("IMG", "Cache render complete");
  return true;
}

}  // namespace

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y) {
  // During the font-cache prewarm pass the framebuffer is discarded, so decoding
  // and drawing the image is pure waste — and an uncached image would do a full
  // (slow, heap-heavy) decode on every scan-pass view, looking like a multi-second
  // hang. Images contribute no glyphs to warm, so skip entirely. (Upstream #2230.)
  if (renderer.isFontCacheScanning()) return;

  LOG_DBG("IMG", "Rendering image at %d,%d: %s (%dx%d)", x, y, imagePath.c_str(), width, height);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Bounds check render position using logical screen dimensions
  if (x < 0 || y < 0 || x + width > screenWidth || y + height > screenHeight) {
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    return;
  }

  // Try to render from cache first
  std::string cachePath = getCachePath(imagePath, ditherMode_);
  if (renderFromCache(renderer, cachePath, x, y, width, height)) {
    return;  // Successfully rendered from cache
  }

  // No cache - need to decode the image
  // Check if image file exists
  FsFile file;
  if (!Storage.openFileForRead("IMG", imagePath, file)) {
    LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
    return;
  }
  size_t fileSize = file.size();
  file.close();

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Decoding and caching: %s", imagePath.c_str());

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.ditherMode = ditherMode_;
  config.performanceMode = false;
  config.useExactDimensions = true;  // Use pre-calculated dimensions to avoid rounding mismatches
  config.cachePath = cachePath;      // Enable caching during decode

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Using %s decoder", decoder->getFormatName());

  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Decode successful");
}

bool ImageBlock::serialize(FsFile& file) {
  serialization::writeString(file, imagePath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(FsFile& file) {
  std::string path;
  serialization::readString(file, path);
  int16_t w, h;
  serialization::readPod(file, w);
  serialization::readPod(file, h);
  auto* ib = new (std::nothrow) ImageBlock(path, w, h);
  if (!ib) {
    LOG_ERR("IMG", "OOM: ImageBlock");
    return nullptr;
  }
  return std::unique_ptr<ImageBlock>(ib);
}
