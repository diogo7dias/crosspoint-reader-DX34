#include "fonts/ReaderFontActivation.h"

#ifdef CROSSPOINT_SD_FONTS
#include "fonts/SdFontManager.h"
#endif

namespace crosspoint {
namespace fonts {

void activateReaderFont(int fontId) {
#ifdef CROSSPOINT_SD_FONTS
  sdFonts().ensureActive(fontId);
#else
  (void)fontId;  // default build: reader fonts are flash-resident, nothing to do
#endif
}

}  // namespace fonts
}  // namespace crosspoint
