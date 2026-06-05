#include "ReaderActivity.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <new>

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

  auto epub = std::unique_ptr<Epub>(new (std::nothrow) Epub(path, Paths::kDataDir));
  if (!epub) {
    LOG_ERR("READER", "OOM new Epub free=%u largest=%u", (unsigned)ESP.getFreeHeap(),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return nullptr;
  }

  uint8_t readerStyleMode = SETTINGS.readerStyleMode;
  ReadingTheme savedBookSettings;
  if (ReadingThemeStore::loadBookSettings(epub->getCachePath(), savedBookSettings)) {
    readerStyleMode = savedBookSettings.readerStyleMode;
  }

  LOG_DBG("HEAP", "READER loadEpub:before free=%u largest=%u min=%u", (unsigned)ESP.getFreeHeap(),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap());
  // Always skip CSS load here. Loading the CSS index eagerly (Wolfe = 33 KB)
  // before the EpubReaderActivity render task is even spawned was the source
  // of the post-PR-#100 OOM screen on heavy books: by the time xTaskCreate
  // runs, largest_free_block has already collapsed below the 8 KB stack.
  // EpubReaderActivity::onEnter calls ensureCssCache() after acquiring its
  // task, so the 33 KB pressure is sequential with the stack alloc rather
  // than simultaneous.
  (void)readerStyleMode;
  if (epub->load(true, /*skipLoadingCss=*/true)) {
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

  auto xtc = std::unique_ptr<Xtc>(new (std::nothrow) Xtc(path, Paths::kDataDir));
  if (!xtc) {
    LOG_ERR("READER", "OOM new Xtc");
    return nullptr;
  }
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

  auto txt = std::unique_ptr<Txt>(new (std::nothrow) Txt(path, Paths::kDataDir));
  if (!txt) {
    LOG_ERR("READER", "OOM new Txt");
    return nullptr;
  }
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
  enterNewActivity(new (std::nothrow) EpubReaderActivity(
      renderer, mappedInput, std::move(epub), [this, epubPath] { goToLibrary(epubPath); }, [this] { onGoBack(); },
      [this](const std::string& path) { openBookPath(path); }));
}

void ReaderActivity::onGoToXtcReader(std::unique_ptr<Xtc> xtc) {
  const auto xtcPath = xtc->getPath();
  ReadingThemeStore::loadBookSettingsIntoCurrent(xtc->getCachePath());
  currentBookPath = xtcPath;
  exitActivity();
  enterNewActivity(new (std::nothrow) XtcReaderActivity(
      renderer, mappedInput, std::move(xtc), [this, xtcPath] { goToLibrary(xtcPath); }, [this] { onGoBack(); },
      [this](const std::string& path) { openBookPath(path); }));
}

void ReaderActivity::onGoToTxtReader(std::unique_ptr<Txt> txt) {
  const auto txtPath = txt->getPath();
  ReadingThemeStore::loadBookSettingsIntoCurrent(txt->getCachePath());
  currentBookPath = txtPath;
  exitActivity();
  enterNewActivity(new (std::nothrow) TxtReaderActivity(
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

  // Draw the "Opening book..." toast here — this is the single funnel every
  // open path passes through: boot-resume and library opens (which arrive via
  // main.cpp openReaderInline -> ReaderActivity::onEnter) AND in-reader opens
  // (the recent-switcher's onOpenBook, which calls this directly and previously
  // showed nothing). resetStacking() first so the still-working threshold timer
  // starts from a fresh timestamp; openReaderInline no longer draws it, so this
  // never double-stacks.
  TransitionFeedback::resetStacking();
  TransitionFeedback::show(renderer, tr(STR_OPENING_BOOK));
  currentBookPath = bookPath;

  if (isXtcFile(bookPath)) {
    auto xtc = loadXtc(bookPath);
    if (xtc) {
      TransitionFeedback::maybeShowStillWorkingToast(renderer);
      onGoToXtcReader(std::move(xtc));
    } else {
      exitActivity();
      enterNewActivity(new (std::nothrow) FullScreenMessageActivity(renderer, mappedInput, tr(STR_LOAD_XTC_FAILED)));
    }
    return;
  }

  if (isQuotesFile(bookPath)) {
    exitActivity();
    enterNewActivity(
        new (std::nothrow) QuotesViewerActivity(renderer, mappedInput, bookPath, [this, bookPath] { goToLibrary(bookPath); }));
    return;
  }

  if (isTxtFile(bookPath)) {
    auto txt = loadTxt(bookPath);
    if (txt) {
      TransitionFeedback::maybeShowStillWorkingToast(renderer);
      onGoToTxtReader(std::move(txt));
    } else {
      exitActivity();
      enterNewActivity(new (std::nothrow) FullScreenMessageActivity(renderer, mappedInput, tr(STR_LOAD_TXT_FAILED)));
    }
    return;
  }

  auto epub = loadEpub(bookPath);
  if (epub) {
    TransitionFeedback::maybeShowStillWorkingToast(renderer);
    onGoToEpubReader(std::move(epub));
  } else {
    exitActivity();
    enterNewActivity(new (std::nothrow) FullScreenMessageActivity(renderer, mappedInput, tr(STR_LOAD_EPUB_FAILED)));
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
