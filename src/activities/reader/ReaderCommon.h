#pragma once

#include <cstdint>
#include <string>

#include "CrossPointState.h"
#include "RecentBooksStore.h"

class GfxRenderer;

namespace ReaderCommon {

// Apply the logical reader orientation to the renderer.
// Shared across reader activities (Epub, Txt) so orientation mapping lives in
// one place.
void applyReaderOrientation(GfxRenderer& renderer, uint8_t orientation);

// Format the status-bar page counter text based on the configured mode.
// Shared between Epub and Txt readers. Epub passes chapter page count here;
// Txt passes its total page count.
std::string formatPageCounterText(uint8_t mode, int currentPage, int totalPages);

// Persist the currently opened book as the last-opened entry and add it to
// the recent books list. Shared by all three reader activities (Epub, Txt,
// Xtc). Callers remain responsible for any subsequent moveBookToRecents()
// step because its post-hook differs per reader.
// Inlined so each reader's call site compiles down to the same sequence it
// previously had, avoiding a cross-translation-unit call overhead.
inline void registerRecentBook(const std::string& path, const std::string& title, const std::string& author,
                               const std::string& thumbPath) {
  APP_STATE.openEpubPath = path;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(path, title, author, thumbPath);
}

}  // namespace ReaderCommon
