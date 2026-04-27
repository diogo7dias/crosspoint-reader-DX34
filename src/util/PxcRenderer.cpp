#include "PxcRenderer.h"

#include <Epub/converters/DirectPixelWriter.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_task_wdt.h>

#include <cstdint>
#include <cstdlib>

namespace PxcRenderer {

bool renderPxc(GfxRenderer& renderer, const std::string& path, GfxRenderer::GrayscaleMode mode) {
  FsFile file;
  if (!Storage.openFileForRead("PXC", path, file)) {
    LOG_ERR("PXC", "Cannot open: %s", path.c_str());
    return false;
  }
  uint16_t pxcWidth = 0, pxcHeight = 0;
  if (file.read(&pxcWidth, 2) != 2 || file.read(&pxcHeight, 2) != 2) {
    LOG_ERR("PXC", "Header read failed: %s", path.c_str());
    file.close();
    return false;
  }
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  if (std::abs(static_cast<int>(pxcWidth) - sw) > 1 ||
      std::abs(static_cast<int>(pxcHeight) - sh) > 1) {
    LOG_ERR("PXC", "Size mismatch %dx%d (screen %dx%d): %s",
            pxcWidth, pxcHeight, sw, sh, path.c_str());
    file.close();
    return false;
  }
  const uint32_t dataOffset = file.position();
  const int bytesPerRow = (pxcWidth + 3) / 4;
  uint8_t* rowBuf = static_cast<uint8_t*>(malloc(bytesPerRow));
  if (!rowBuf) {
    LOG_ERR("PXC", "Row alloc failed (%d bytes)", bytesPerRow);
    file.close();
    return false;
  }

  const int width = static_cast<int>(pxcWidth);
  const int height = static_cast<int>(pxcHeight);
  renderer.renderGrayscale(mode, [&]() {
    file.seek(dataOffset);
    DirectPixelWriter pw;
    pw.init(renderer);
    for (int row = 0; row < height; row++) {
      if (file.read(rowBuf, bytesPerRow) != bytesPerRow) break;
      pw.beginRow(row);
      for (int col = 0; col < width; col++) {
        const uint8_t pv = (rowBuf[col >> 2] >> (6 - (col & 3) * 2)) & 0x03;
        pw.writePixel(col, pv);
      }
      if ((row & 31) == 0) esp_task_wdt_reset();
    }
  });

  free(rowBuf);
  file.close();
  return true;
}

}  // namespace PxcRenderer
