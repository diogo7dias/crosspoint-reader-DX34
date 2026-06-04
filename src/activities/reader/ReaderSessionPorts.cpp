#include "ReaderSessionPorts.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "ReaderCommon.h"
#include "RecentBooksStore.h"

namespace crosspoint {
namespace reader {

void ProdDisplayPort::requestRefresh(bool full) {
  if (full) {
    renderer_.requestFullRefresh();
  } else {
    renderer_.requestHalfRefresh();
  }
}

void ProdDisplayPort::applyOrientationFromSettings() {
  ReaderCommon::applyReaderOrientation(renderer_, SETTINGS.orientation);
}

void ProdDisplayPort::setBoldSwap(bool enabled) { EpdFontFamily::setReaderBoldSwapEnabled(enabled); }

bool ProdEnvPort::shouldFullRefreshOnEnter(const std::string& path) {
  return ReaderCommon::shouldFullRefreshOnEnter(path);
}

bool ProdEnvPort::boldSwap(const std::string& path) const { return RECENT_BOOKS.getBoldSwap(path); }

void ProdEnvPort::registerOpened(const std::string& path, const std::string& title, const std::string& author,
                                 const std::string& thumb) {
  ReaderCommon::registerRecentBook(path, title, author, thumb);
}

std::string ProdEnvPort::moveBookToRecents(const std::string& path) { return RECENT_BOOKS.moveBookToRecents(path); }

}  // namespace reader
}  // namespace crosspoint
