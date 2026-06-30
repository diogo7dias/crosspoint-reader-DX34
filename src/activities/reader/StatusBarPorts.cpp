#include "StatusBarPorts.h"

#include <GfxRenderer.h>

namespace crosspoint {
namespace reader {

int ProdStatusMeasurePort::getScreenWidth() const { return renderer_.getScreenWidth(); }
int ProdStatusMeasurePort::getScreenHeight() const { return renderer_.getScreenHeight(); }
int ProdStatusMeasurePort::getLineHeight(const int fontId) const { return renderer_.getLineHeight(fontId); }
int ProdStatusMeasurePort::getTextHeight(const int fontId) const { return renderer_.getTextHeight(fontId); }
int ProdStatusMeasurePort::getTextWidth(const int fontId, const char* text) const {
  return renderer_.getTextWidth(fontId, text);
}
std::string ProdStatusMeasurePort::truncatedText(const int fontId, const char* text, const int maxWidth) const {
  return renderer_.truncatedText(fontId, text, maxWidth);
}
void ProdStatusMeasurePort::getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const {
  renderer_.getOrientedViewableTRBL(outTop, outRight, outBottom, outLeft);
}

}  // namespace reader
}  // namespace crosspoint
