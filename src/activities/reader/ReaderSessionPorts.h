/**
 * @file ReaderSessionPorts.h
 * @brief Production adapters binding ReaderSession's ports to the real globals.
 *
 * Shared by all three readers (the wiring is identical — only the ReaderHooks +
 * IProgressSink differ per reader). Device-only: pulls GfxRenderer / SETTINGS /
 * RECENT_BOOKS / ReaderCommon / EpdFontFamily. Host tests use in-memory fakes
 * instead (see test/test_reader_session). RFC #171.
 */
#pragma once

#include "ReaderSession.h"

class GfxRenderer;

namespace crosspoint {
namespace reader {

// GfxRenderer + ReaderCommon::applyReaderOrientation + EpdFontFamily bold-swap.
class ProdDisplayPort : public IReaderDisplayPort {
 public:
  explicit ProdDisplayPort(GfxRenderer& renderer) : renderer_(renderer) {}
  void requestRefresh(bool full) override;
  void applyOrientationFromSettings() override;
  void setBoldSwap(bool enabled) override;

 private:
  GfxRenderer& renderer_;
};

// ReaderCommon refresh policy + RECENT_BOOKS + APP_STATE recent registration.
class ProdEnvPort : public IReaderEnvPort {
 public:
  bool shouldFullRefreshOnEnter(const std::string& path) override;
  bool boldSwap(const std::string& path) const override;
  void registerOpened(const std::string& path, const std::string& title, const std::string& author,
                      const std::string& thumb) override;
  std::string moveBookToRecents(const std::string& path) override;
};

}  // namespace reader
}  // namespace crosspoint
