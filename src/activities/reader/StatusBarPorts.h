#pragma once

#include "IStatusMeasurePort.h"

/**
 * @file StatusBarPorts.h
 * @brief Production adapter binding IStatusMeasurePort to the live GfxRenderer.
 *
 * Device-only translation unit (mirrors ReaderSessionPorts.h ProdDisplayPort).
 * Forwards each measurement call 1:1 to the wrapped renderer; carries no state
 * beyond the reference, so constructing one per call is free (stack, no heap).
 */
class GfxRenderer;

namespace crosspoint {
namespace reader {

class ProdStatusMeasurePort final : public IStatusMeasurePort {
 public:
  explicit ProdStatusMeasurePort(const GfxRenderer& renderer) : renderer_(renderer) {}

  int getScreenWidth() const override;
  int getScreenHeight() const override;
  int getLineHeight(int fontId) const override;
  int getTextHeight(int fontId) const override;
  int getTextWidth(int fontId, const char* text) const override;
  std::string truncatedText(int fontId, const char* text, int maxWidth) const override;
  void getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const override;

 private:
  const GfxRenderer& renderer_;
};

}  // namespace reader
}  // namespace crosspoint
