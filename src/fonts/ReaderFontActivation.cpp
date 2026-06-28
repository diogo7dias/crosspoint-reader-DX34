#include "fonts/ReaderFontActivation.h"

#ifdef CROSSPOINT_SD_FONTS
#include "CrossPointSettings.h"
#include "fonts/SdFontManager.h"
#endif

namespace crosspoint {
namespace fonts {

void activateReaderFont(int fontId) {
#ifdef CROSSPOINT_SD_FONTS
  sdFonts().ensureActive(fontId);
  if (sdFonts().activeFellBack()) {
    // The requested reader font has no usable SD pack (slim build: not downloaded,
    // corrupt, or it couldn't be opened under low memory). The manager already
    // substituted a flash fallback; latch the existing emergency-downgrade so
    // getReaderFontId() returns a real flash font id. Render + layout then happen
    // under that id (not the requested one), so the section cache stays consistent
    // and self-heals once the pack is available (the latch clears on book close).
    SETTINGS.emergencyRenderFontDowngrade = true;
  }
#else
  (void)fontId;  // default build: reader fonts are flash-resident, nothing to do
#endif
}

}  // namespace fonts
}  // namespace crosspoint
