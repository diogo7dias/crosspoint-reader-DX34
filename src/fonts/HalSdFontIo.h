#pragma once

#ifdef CROSSPOINT_SD_FONTS

#include "fonts/SdFontManager.h"

namespace crosspoint {
namespace fonts {

// Device implementation of the SdFontManager filesystem seam. CPBN font packs
// live on the SD card, served through HalStorage / HalFile and written with the
// EpdBinExport serializer. Kept out of the header-only manager so the manager
// itself stays HAL-free and host-testable.
class HalSdFontIo : public SdFontIo {
 public:
  bool exists(const char* path) override;
  bool exportBlob(const char* path, const EpdFontData& flashData, uint16_t sizePt) override;
  binfont::BlobSource* openSource(const char* path) override;
  void releaseSource(binfont::BlobSource* src) override;
};

}  // namespace fonts
}  // namespace crosspoint

#endif  // CROSSPOINT_SD_FONTS
