#pragma once

#include <string>

/**
 * @file IStatusMeasurePort.h
 * @brief Hardware-free text-measurement seam for the reader status-bar pipeline.
 *
 * The status-bar reserve/build logic only needs to MEASURE text and query
 * screen geometry — it never draws. Today those measurements come straight off
 * GfxRenderer, which owns a HalDisplay, so the pure string-selection /
 * reserved-height arithmetic cannot run off-device and is untested.
 *
 * This port abstracts exactly the measurement calls the status-bar helpers use.
 * Production wraps the live GfxRenderer (ProdStatusMeasurePort, in
 * StatusBarPorts.h); host tests inject a deterministic fake. Method names mirror
 * GfxRenderer 1:1 so the production adapter is a pure forward.
 *
 * Mirrors the IReaderDisplayPort / ProdDisplayPort split in ReaderSession.h /
 * ReaderSessionPorts.h. Dependency category: Local-substitutable.
 */
namespace crosspoint {
namespace reader {

struct IStatusMeasurePort {
  virtual ~IStatusMeasurePort() = default;

  virtual int getScreenWidth() const = 0;
  virtual int getScreenHeight() const = 0;
  virtual int getLineHeight(int fontId) const = 0;
  virtual int getTextHeight(int fontId) const = 0;
  virtual int getTextWidth(int fontId, const char* text) const = 0;
  virtual std::string truncatedText(int fontId, const char* text, int maxWidth) const = 0;
  virtual void getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const = 0;
};

}  // namespace reader
}  // namespace crosspoint
