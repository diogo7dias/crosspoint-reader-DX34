#include "fonts/ReaderFontActivation.h"

namespace crosspoint {
namespace fonts {

// Lector is a flash-only font build (the SD-backed CPBN pack system was removed),
// so there is nothing to activate: every reader font's bitmaps are resident in
// flash. Kept as a no-op so the reader activities can keep calling it
// unconditionally without pulling in any SD-font machinery.
void activateReaderFont(int fontId) { (void)fontId; }

}  // namespace fonts
}  // namespace crosspoint
