#include "ReaderActivity.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include "CrossPointSettings.h"
#include "Epub.h"
#include "EpubReaderActivity.h"
#include "Paths.h"
#include "QuotesViewerActivity.h"
#include "ReadingThemeStore.h"
#include "Txt.h"
#include "TxtReaderActivity.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "util/StringUtils.h"
#include "util/TransitionFeedback.h"

std::string ReaderActivity::extractFolderPath(const std::string& filePath) {
  const auto lastSlash = filePath.find_last_of('/');
  if (lastSlash == std::string::npos || lastSlash == 0) {
    return "/";
  }
  return filePath.substr(0, lastSlash);
}

bool ReaderActivity::isXtcFile(const std::string& path) {
  return StringUtils::checkFileExtension(path, ".xtc") || StringUtils::checkFileExtension(path, ".xtch");
}

bool ReaderActivity::isTxtFile(const std::string& path) {
  return StringUtils::checkFileExtension(path, ".txt") ||
         StringUtils::checkFileExtension(path, ".md");  // Treat .md as txt files (until we have a markdown reader)
}

bool ReaderActivity::isQuotesFile(const std::string& path) {
  return path.size() >= 11 && path.compare(path.size() - 11, 11, "_QUOTES.txt") == 0;
}

std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto epub = std::unique_ptr<Epub>(new Epub(path, Paths::kDataDir));

  uint8_t readerStyleMode = SETTINGS.readerStyleMode;
  ReadingTheme savedBookSettings;
  if (ReadingThemeStore::loadBookSettings(epub->getCachePath(), savedBookSettings)) {
    readerStyleMode = savedBookSettings.readerStyleMode;
  }

  LOG_DBG("HEAP", "READER loadEpub:before free=%u largest=%u min=%u", (unsigned)ESP.getFreeHeap(),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap());
  if (epub->load(true, readerStyleMode == CrossPointSettings::READER_STYLE_USER)) {
    LOG_DBG("HEAP", "READER loadEpub:after-ok free=%u largest=%u min=%u", (unsigned)ESP.getFreeHeap(),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap());
    return epub;
  }

  LOG_ERR("READER", "Failed to load epub");
  LOG_DBG("HEAP", "READER loadEpub:after-fail free=%u largest=%u min=%u", (unsigned)ESP.getFreeHeap(),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap());
  return nullptr;
}

std::unique_ptr<Xtc> ReaderActivity::loadXtc(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto xtc = std::unique_ptr<Xtc>(new Xtc(path, Paths::kDataDir));
  if (xtc->load()) {
    return xtc;
  }

  LOG_ERR("READER", "Failed to load XTC");
  return nullptr;
}

std::unique_ptr<Txt> ReaderActivity::loadTxt(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto txt = std::unique_ptr<Txt>(new Txt(path, Paths::kDataDir));
  if (txt->load()) {
    return txt;
  }

  LOG_ERR("READER", "Failed to load TXT");
  return nullptr;
}

void ReaderActivity::goToLibrary(const std::string& fromBookPath) {
  // Block 2 (v1.2.0): half refresh on book exit scrubs the page ghost under
  // the library list and is ~1 s faster than FULL. Experimental; revert to
  // requestFullRefresh() if ghost artifacts appear.
  renderer.requestHalfRefresh();
  // If coming from a book, start in that book's folder; otherwise start from root
  const auto initialPath = fromBookPath.empty() ? "/" : extractFolderPath(fromBookPath);
  onGoToLibrary(initialPath);
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub) {
  const auto epubPath = epub->getPath();
  ReadingThemeStore::loadBookSettingsIntoCurrent(epub->getCachePath());
  currentBookPath = epubPath;
  exitActivity();
  enterNewActivity(new EpubReaderActivity(
      renderer, mappedInput, std::move(epub), [this, epubPath] { goToLibrary(epubPath); }, [this] { onGoBack(); },
      [this](const std::string& path) { openBookPath(path); }));
}

void ReaderActivity::onGoToXtcReader(std::unique_ptr<Xtc> xtc) {
  const auto xtcPath = xtc->getPath();
  currentBookPath = xtcPath;
  exitActivity();
  enterNewActivity(new XtcReaderActivity(
      renderer, mappedInput, std::move(xtc), [this, xtcPath] { goToLibrary(xtcPath); }, [this] { onGoBack(); },
      [this](const std::string& path) { openBookPath(path); }));
}

void ReaderActivity::onGoToTxtReader(std::unique_ptr<Txt> txt) {
  const auto txtPath = txt->getPath();
  ReadingThemeStore::loadBookSettingsIntoCurrent(txt->getCachePath());
  currentBookPath = txtPath;
  exitActivity();
  enterNewActivity(new TxtReaderActivity(
      renderer, mappedInput, std::move(txt), [this, txtPath] { goToLibrary(txtPath); }, [this] { onGoBack(); },
      [this](const std::string& path) { openBookPath(path); }));
}

void ReaderActivity::openBookPath(const std::string& bookPath) {
  if (bookPath.empty()) {
    return;
  }

  // Restore global reader settings before loading book.
  // Per-book overrides (if any) are applied later in onGoTo*Reader.
  // This ensures new books always start with the user's global defaults,
  // not stale settings from whatever book was open previously.
  SETTINGS.loadFromFile();

  // The "Opening book..." toast was already drawn by openReaderInline() in
  // main.cpp before this activity was entered — don't redraw it here or it
  // stacks twice. The still-working threshold timer starts automatically
  // from the first toast's draw time, so no explicit start call is needed.
  currentBookPath = bookPath;

  if (isXtcFile(bookPath)) {
    auto xtc = loadXtc(bookPath);
    if (xtc) {
      TransitionFeedback::maybeShowStillWorkingToast(renderer);
      onGoToXtcReader(std::move(xtc));
    } else {
      exitActivity();
      enterNewActivity(new FullScreenMessageActivity(renderer, mappedInput, tr(STR_LOAD_XTC_FAILED)));
    }
    return;
  }

  if (isQuotesFile(bookPath)) {
    exitActivity();
    enterNewActivity(
        new QuotesViewerActivity(renderer, mappedInput, bookPath, [this, bookPath] { goToLibrary(bookPath); }));
    return;
  }

  if (isTxtFile(bookPath)) {
    auto txt = loadTxt(bookPath);
    if (txt) {
      TransitionFeedback::maybeShowStillWorkingToast(renderer);
      onGoToTxtReader(std::move(txt));
    } else {
      exitActivity();
      enterNewActivity(new FullScreenMessageActivity(renderer, mappedInput, tr(STR_LOAD_TXT_FAILED)));
    }
    return;
  }

  auto epub = loadEpub(bookPath);
  if (epub) {
    TransitionFeedback::maybeShowStillWorkingToast(renderer);
    onGoToEpubReader(std::move(epub));
  } else {
    exitActivity();
    enterNewActivity(new FullScreenMessageActivity(renderer, mappedInput, tr(STR_LOAD_EPUB_FAILED)));
  }
}

void ReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (initialBookPath.empty()) {
    goToLibrary();  // Start from root when entering via Browse
    return;
  }
  openBookPath(initialBookPath);
  if (!subActivity) {
    // Book failed to load and no error screen was shown — go to library at book location
    goToLibrary(initialBookPath);
  }
}
