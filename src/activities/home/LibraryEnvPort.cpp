#include "LibraryEnvPort.h"

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"

namespace crosspoint {
namespace home {

bool ProdLibraryEnvPort::showHiddenFiles() const { return SETTINGS.showHiddenFiles; }

bool ProdLibraryEnvPort::shuffleBooksFolder() const { return SETTINGS.booksFolderOrder == 1; }

int ProdLibraryEnvPort::cachedPercent(const std::string& path) const { return RECENT_BOOKS.getCachedPercent(path); }

void ProdLibraryEnvPort::removeRecent(const std::string& path) { RECENT_BOOKS.removeBook(path); }

void ProdLibraryEnvPort::persistState() { APP_STATE.saveToFile(); }

ILibraryEnvPort& defaultLibraryEnv() {
  static ProdLibraryEnvPort instance;
  return instance;
}

}  // namespace home
}  // namespace crosspoint
