#include "Epub.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>
#include <ZipFile.h>
#include <esp_heap_caps.h>

#include <algorithm>

// Temporary (fix/fonts-stranded-largest branch): include `largest` so we can
// trace where the top contiguous block collapses across the epub-load
// lifecycle. `free` tells you how much RAM is unused; `largest` tells you how
// much of it is in one piece — which is what matters for the 32 KB ZIP dict.
#define LOG_HEAP(label)                                                                                               \
  LOG_DBG("HEAP", "EBP %s free=%u largest=%u min=%u", label, (unsigned)ESP.getFreeHeap(),                             \
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap())

#include "Epub/parsers/ContainerParser.h"
#include "Epub/parsers/ContentOpfParser.h"
#include "Epub/parsers/TocNavParser.h"
#include "Epub/parsers/TocNcxParser.h"

namespace {

constexpr int kProgressStepPercent = 5;

class ProgressReporter {
 public:
  explicit ProgressReporter(const std::function<void(int)>& callback) : callback(callback) {}

  void report(const int percent) {
    if (!callback) {
      return;
    }
    const int clamped = std::max(0, std::min(100, percent));
    if (clamped <= lastPercent) {
      return;
    }

    const int throttled = (clamped == 100) ? 100 : (clamped / kProgressStepPercent) * kProgressStepPercent;
    if (throttled <= lastPercent) {
      return;
    }

    lastPercent = throttled;
    callback(lastPercent);
  }

  int current() const { return lastPercent; }

 private:
  std::function<void(int)> callback;
  int lastPercent = -1;
};

class ProgressSpan {
 public:
  ProgressSpan(ProgressReporter& reporter, const int startPercent, const int endPercent)
      : reporter(reporter), startPercent(startPercent), endPercent(endPercent) {
    reporter.report(startPercent);
  }

  void reportPercent(const int percent) const {
    const int clamped = std::max(0, std::min(100, percent));
    const int mapped = startPercent + ((endPercent - startPercent) * clamped) / 100;
    reporter.report(mapped);
  }

  void reportFraction(const size_t numerator, const size_t denominator) const {
    if (denominator == 0) {
      reportPercent(100);
      return;
    }
    reportPercent(static_cast<int>((numerator * 100U) / denominator));
  }

  void finish() const { reporter.report(endPercent); }

 private:
  ProgressReporter& reporter;
  int startPercent;
  int endPercent;
};

}  // namespace

bool Epub::findContentOpfFile(std::string* contentOpfFile) const {
  const auto containerPath = "META-INF/container.xml";
  size_t containerSize;

  // Get file size without loading it all into heap
  if (!getItemSize(containerPath, &containerSize)) {
    LOG_ERR("EBP", "Could not find or size META-INF/container.xml");
    return false;
  }

  ContainerParser containerParser(containerSize);

  if (!containerParser.setup()) {
    return false;
  }

  // Stream read (reusing your existing stream logic)
  if (!readItemContentsToStream(containerPath, containerParser, 512)) {
    LOG_ERR("EBP", "Could not read META-INF/container.xml");
    return false;
  }

  // Extract the result
  if (containerParser.fullPath.empty()) {
    LOG_ERR("EBP", "Could not find valid rootfile in container.xml");
    return false;
  }

  *contentOpfFile = std::move(containerParser.fullPath);
  return true;
}

bool Epub::parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata) {
  std::string contentOpfFilePath;
  if (!findContentOpfFile(&contentOpfFilePath)) {
    LOG_ERR("EBP", "Could not find content.opf in zip");
    return false;
  }

  contentBasePath = contentOpfFilePath.substr(0, contentOpfFilePath.find_last_of('/') + 1);

  LOG_DBG("EBP", "Parsing content.opf: %s", contentOpfFilePath.c_str());

  size_t contentOpfSize;
  if (!getItemSize(contentOpfFilePath, &contentOpfSize)) {
    LOG_ERR("EBP", "Could not get size of content.opf");
    return false;
  }

  ContentOpfParser opfParser(getCachePath(), getBasePath(), contentOpfSize, bookMetadataCache.get());
  if (!opfParser.setup()) {
    LOG_ERR("EBP", "Could not setup content.opf parser");
    return false;
  }

  if (!readItemContentsToStream(contentOpfFilePath, opfParser, 1024)) {
    LOG_ERR("EBP", "Could not read content.opf");
    return false;
  }

  // Grab data from opfParser into epub
  bookMetadata.title = opfParser.title;
  bookMetadata.author = opfParser.author;
  bookMetadata.language = opfParser.language;
  bookMetadata.coverItemHref = opfParser.coverItemHref;

  // Guide-based cover fallback: if no cover found via metadata/properties,
  // try extracting the image reference from the guide's cover page XHTML
  if (bookMetadata.coverItemHref.empty() && !opfParser.guideCoverPageHref.empty()) {
    LOG_DBG("EBP", "No cover from metadata, trying guide cover page: %s", opfParser.guideCoverPageHref.c_str());
    size_t coverPageSize;
    uint8_t* coverPageData = readItemContentsToBytes(opfParser.guideCoverPageHref, &coverPageSize, true);
    if (coverPageData) {
      const std::string coverPageHtml(reinterpret_cast<char*>(coverPageData), coverPageSize);
      free(coverPageData);

      // Determine base path of the cover page for resolving relative image references
      std::string coverPageBase;
      const auto lastSlash = opfParser.guideCoverPageHref.rfind('/');
      if (lastSlash != std::string::npos) {
        coverPageBase = opfParser.guideCoverPageHref.substr(0, lastSlash + 1);
      }

      // Search for image references: xlink:href="..." (SVG) and src="..." (img)
      std::string imageRef;
      for (const char* pattern : {"xlink:href=\"", "src=\""}) {
        auto pos = coverPageHtml.find(pattern);
        while (pos != std::string::npos) {
          pos += strlen(pattern);
          const auto endPos = coverPageHtml.find('"', pos);
          if (endPos != std::string::npos) {
            const auto ref = coverPageHtml.substr(pos, endPos - pos);
            // Check if it's an image file
            if (ref.length() >= 4) {
              const auto ext = ref.substr(ref.length() - 4);
              if (ext == ".png" || ext == ".jpg" || ext == "jpeg" || ext == ".gif") {
                imageRef = ref;
                break;
              }
            }
          }
          pos = coverPageHtml.find(pattern, pos);
        }
        if (!imageRef.empty()) break;
      }

      if (!imageRef.empty()) {
        bookMetadata.coverItemHref = FsHelpers::normalisePath(coverPageBase + imageRef);
        LOG_DBG("EBP", "Found cover image from guide: %s", bookMetadata.coverItemHref.c_str());
      }
    }
  }

  bookMetadata.textReferenceHref = opfParser.textReferenceHref;

  if (!opfParser.tocNcxPath.empty()) {
    tocNcxItem = opfParser.tocNcxPath;
  }

  if (!opfParser.tocNavPath.empty()) {
    tocNavItem = opfParser.tocNavPath;
  }

  if (!opfParser.cssFiles.empty()) {
    cssFiles = opfParser.cssFiles;
  }

  LOG_DBG("EBP", "Successfully parsed content.opf");
  return true;
}

bool Epub::parseTocNcxFile() const {
  // the ncx file should have been specified in the content.opf file
  if (tocNcxItem.empty()) {
    LOG_DBG("EBP", "No ncx file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc ncx file: %s", tocNcxItem.c_str());

  const auto tmpNcxPath = getCachePath() + "/toc.ncx";
  FsFile tempNcxFile;
  if (!Storage.openFileForWrite("EBP", tmpNcxPath, tempNcxFile)) {
    return false;
  }
  readItemContentsToStream(tocNcxItem, tempNcxFile, 1024);
  tempNcxFile.close();
  if (!Storage.openFileForRead("EBP", tmpNcxPath, tempNcxFile)) {
    return false;
  }
  const auto ncxSize = tempNcxFile.size();

  TocNcxParser ncxParser(contentBasePath, ncxSize, bookMetadataCache.get());

  if (!ncxParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc ncx parser");
    tempNcxFile.close();
    return false;
  }

  const auto ncxBuffer = static_cast<uint8_t*>(malloc(1024));
  if (!ncxBuffer) {
    LOG_ERR("EBP", "Could not allocate memory for toc ncx parser");
    tempNcxFile.close();
    return false;
  }

  while (tempNcxFile.available()) {
    const auto readSize = tempNcxFile.read(ncxBuffer, 1024);
    if (readSize == 0) break;
    const auto processedSize = ncxParser.write(ncxBuffer, readSize);

    if (processedSize != readSize) {
      LOG_ERR("EBP", "Could not process all toc ncx data");
      free(ncxBuffer);
      tempNcxFile.close();
      return false;
    }
    if (progressCallback && ncxSize > 0) {
      progressCallback(static_cast<int>((tempNcxFile.position() * 100U) / ncxSize));
    }
  }

  free(ncxBuffer);
  tempNcxFile.close();
  Storage.remove(tmpNcxPath.c_str());

  LOG_DBG("EBP", "Parsed TOC items");
  return true;
}

bool Epub::parseTocNavFile() const {
  // the nav file should have been specified in the content.opf file (EPUB 3)
  if (tocNavItem.empty()) {
    LOG_DBG("EBP", "No nav file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc nav file: %s", tocNavItem.c_str());

  const auto tmpNavPath = getCachePath() + "/toc.nav";
  FsFile tempNavFile;
  if (!Storage.openFileForWrite("EBP", tmpNavPath, tempNavFile)) {
    return false;
  }
  readItemContentsToStream(tocNavItem, tempNavFile, 1024);
  tempNavFile.close();
  if (!Storage.openFileForRead("EBP", tmpNavPath, tempNavFile)) {
    return false;
  }
  const auto navSize = tempNavFile.size();

  // Note: We can't use `contentBasePath` here as the nav file may be in a different folder to the content.opf
  // and the HTMLX nav file will have hrefs relative to itself
  const std::string navContentBasePath = tocNavItem.substr(0, tocNavItem.find_last_of('/') + 1);
  TocNavParser navParser(navContentBasePath, navSize, bookMetadataCache.get());

  if (!navParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc nav parser");
    return false;
  }

  const auto navBuffer = static_cast<uint8_t*>(malloc(1024));
  if (!navBuffer) {
    LOG_ERR("EBP", "Could not allocate memory for toc nav parser");
    return false;
  }

  while (tempNavFile.available()) {
    const auto readSize = tempNavFile.read(navBuffer, 1024);
    const auto processedSize = navParser.write(navBuffer, readSize);

    if (processedSize != readSize) {
      LOG_ERR("EBP", "Could not process all toc nav data");
      free(navBuffer);
      tempNavFile.close();
      return false;
    }
    if (progressCallback && navSize > 0) {
      progressCallback(static_cast<int>((tempNavFile.position() * 100U) / navSize));
    }
  }

  free(navBuffer);
  tempNavFile.close();
  Storage.remove(tmpNavPath.c_str());

  LOG_DBG("EBP", "Parsed TOC nav items");
  return true;
}

void Epub::parseCssFiles() const {
  // Maximum CSS file size we'll attempt to parse (uncompressed)
  // Larger files risk memory exhaustion on ESP32
  constexpr size_t MAX_CSS_FILE_SIZE = 128 * 1024;  // 128KB
  // Minimum heap required before attempting CSS parsing
  constexpr size_t MIN_HEAP_FOR_CSS_PARSING = 64 * 1024;  // 64KB

  if (cssFiles.empty()) {
    LOG_DBG("EBP", "No CSS files to parse, but CssParser created for inline styles");
  }

  LOG_DBG("EBP", "CSS files to parse: %zu", cssFiles.size());

  // See if we have a cached version of the CSS rules
  if (cssParser->hasCache()) {
    LOG_DBG("EBP", "CSS cache exists, skipping parseCssFiles");
    return;
  }

  // No cache yet - parse CSS files
  const size_t totalCssFiles = std::max<size_t>(cssFiles.size(), 1);
  size_t parsedCssFiles = 0;
  for (const auto& cssPath : cssFiles) {
    LOG_DBG("EBP", "Parsing CSS file: %s", cssPath.c_str());

    // Check heap before parsing - CSS parsing allocates heavily
    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < MIN_HEAP_FOR_CSS_PARSING) {
      LOG_ERR("EBP", "Insufficient heap for CSS parsing (%u bytes free, need %zu), skipping: %s", freeHeap,
              MIN_HEAP_FOR_CSS_PARSING, cssPath.c_str());
      continue;
    }

    // Check CSS file size before decompressing - skip files that are too large
    size_t cssFileSize = 0;
    if (getItemSize(cssPath, &cssFileSize)) {
      if (cssFileSize > MAX_CSS_FILE_SIZE) {
        LOG_ERR("EBP", "CSS file too large (%zu bytes > %zu max), skipping: %s", cssFileSize, MAX_CSS_FILE_SIZE,
                cssPath.c_str());
        parsedCssFiles++;
        if (progressCallback) {
          progressCallback(static_cast<int>((parsedCssFiles * 100U) / totalCssFiles));
        }
        continue;
      }
    }

    // Extract CSS file to temp location
    const auto tmpCssPath = getCachePath() + "/.tmp.css";
    FsFile tempCssFile;
    if (!Storage.openFileForWrite("EBP", tmpCssPath, tempCssFile)) {
      LOG_ERR("EBP", "Could not create temp CSS file");
      continue;
    }
    if (!readItemContentsToStream(cssPath, tempCssFile, 1024)) {
      LOG_ERR("EBP", "Could not read CSS file: %s", cssPath.c_str());
      tempCssFile.close();
      Storage.remove(tmpCssPath.c_str());
      parsedCssFiles++;
      if (progressCallback) {
        progressCallback(static_cast<int>((parsedCssFiles * 100U) / totalCssFiles));
      }
      continue;
    }
    tempCssFile.close();

    // Parse the CSS file
    if (!Storage.openFileForRead("EBP", tmpCssPath, tempCssFile)) {
      LOG_ERR("EBP", "Could not open temp CSS file for reading");
      Storage.remove(tmpCssPath.c_str());
      continue;
    }
    cssParser->loadFromStream(tempCssFile);
    tempCssFile.close();
    Storage.remove(tmpCssPath.c_str());
    parsedCssFiles++;
    if (progressCallback) {
      progressCallback(static_cast<int>((parsedCssFiles * 100U) / totalCssFiles));
    }
  }

  // Save to cache for next time
  if (!cssParser->saveToCache()) {
    LOG_ERR("EBP", "Failed to save CSS rules to cache");
  }
  cssParser->clear();

  LOG_DBG("EBP", "Loaded %zu CSS style rules from %zu files", cssParser->ruleCount(), cssFiles.size());
}

// load in the meta data for the epub file
bool Epub::load(const bool buildIfMissing, const bool skipLoadingCss,
                const std::function<void(int)>& progressCallback) {
  LOG_DBG("EBP", "Loading ePub: %s", filepath.c_str());
  LOG_HEAP("load:start");
  this->progressCallback = progressCallback;
  ProgressReporter reporter(progressCallback);

  // Initialize spine/TOC cache
  bookMetadataCache.reset(new BookMetadataCache(cachePath));
  // Always create CssParser - needed for inline style parsing even without CSS files
  cssParser.reset(new CssParser(cachePath));
  reporter.report(0);

  // Try to load existing cache first
  {
    ProgressSpan cacheProbeSpan(reporter, 0, 10);
    cacheProbeSpan.reportPercent(30);
    if (bookMetadataCache->load([&cacheProbeSpan](const int percent) { cacheProbeSpan.reportPercent(percent); })) {
      cacheProbeSpan.finish();
      LOG_HEAP("load:cache-hit");
      LOG_DBG("EBP", "cache-hit spineCount=%d tocCount=%d", bookMetadataCache->getSpineCount(),
              bookMetadataCache->getTocCount());
      if (!skipLoadingCss) {
        ProgressSpan cachedCssSpan(reporter, 10, 90);
        cachedCssSpan.reportPercent(15);
        // Rebuild CSS cache when missing or when cache version changed (loadFromCache removes stale file)
        if (!cssParser->hasCache() || !cssParser->loadFromCache()) {
          LOG_DBG("EBP", "CSS rules cache missing or stale, attempting to parse CSS files");
          LOG_HEAP("cache-hit:css-rebuild-start");
          cssParser->deleteCache();

          ProgressSpan cachedOpfSpan(reporter, 10, 35);
          if (!parseContentOpf(bookMetadataCache->coreMetadata)) {
            LOG_ERR("EBP", "Could not parse content.opf from cached bookMetadata for CSS files");
            // continue anyway - book will work without CSS and we'll still load any inline style CSS
          }
          cachedOpfSpan.finish();

          ProgressSpan cssSpan(reporter, 80, 90);
          parseCssFiles();
          cssSpan.finish();
          LOG_HEAP("cache-hit:css-rebuild-done");
        } else {
          cachedCssSpan.finish();
          LOG_HEAP("cache-hit:css-loaded");
        }
      } else {
        reporter.report(90);
      }
      LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
      LOG_HEAP("load:done-cache-hit");
      return true;
    }
  }

  reporter.report(10);
  LOG_HEAP("load:cache-miss-build-start");

  // If we didn't load from cache above and we aren't allowed to build, fail now
  if (!buildIfMissing) {
    return false;
  }

  // Cache doesn't exist or is invalid, build it
  LOG_DBG("EBP", "Cache not found, building spine/TOC cache");
  setupCacheDir();

  const uint32_t indexingStart = millis();

  // Begin building cache - stream entries to disk immediately
  if (!bookMetadataCache->beginWrite()) {
    LOG_ERR("EBP", "Could not begin writing cache");
    return false;
  }

  // OPF Pass
  const uint32_t opfStart = millis();
  BookMetadataCache::BookMetadata bookMetadata;
  if (!bookMetadataCache->beginContentOpfPass()) {
    LOG_ERR("EBP", "Could not begin writing content.opf pass");
    return false;
  }
  {
    ProgressSpan opfSpan(reporter, 10, 35);
    opfSpan.reportPercent(10);
    if (!parseContentOpf(bookMetadata)) {
      LOG_ERR("EBP", "Could not parse content.opf");
      return false;
    }
    opfSpan.finish();
  }
  if (!bookMetadataCache->endContentOpfPass()) {
    LOG_ERR("EBP", "Could not end writing content.opf pass");
    return false;
  }
  LOG_DBG("EBP", "OPF pass completed in %lu ms", millis() - opfStart);
  LOG_HEAP("build:opf-done");

  // TOC Pass - try EPUB 3 nav first, fall back to NCX
  const uint32_t tocStart = millis();
  if (!bookMetadataCache->beginTocPass()) {
    LOG_ERR("EBP", "Could not begin writing toc pass");
    return false;
  }

  bool tocParsed = false;

  {
    ProgressSpan tocSpan(reporter, 35, 60);
    tocSpan.reportPercent(5);

    // Try EPUB 3 nav document first (preferred)
    if (!tocNavItem.empty()) {
      LOG_DBG("EBP", "Attempting to parse EPUB 3 nav document");
      tocParsed = parseTocNavFile();
    }

    // Fall back to NCX if nav parsing failed or wasn't available
    if (!tocParsed && !tocNcxItem.empty()) {
      LOG_DBG("EBP", "Falling back to NCX TOC");
      tocParsed = parseTocNcxFile();
    }

    tocSpan.finish();
  }

  if (!tocParsed) {
    LOG_ERR("EBP", "Warning: Could not parse any TOC format");
    // Continue anyway - book will work without TOC
  }

  if (!bookMetadataCache->endTocPass()) {
    LOG_ERR("EBP", "Could not end writing toc pass");
    return false;
  }
  LOG_DBG("EBP", "TOC pass completed in %lu ms", millis() - tocStart);
  LOG_HEAP("build:toc-done");

  // Close the cache files
  if (!bookMetadataCache->endWrite()) {
    LOG_ERR("EBP", "Could not end writing cache");
    return false;
  }

  // Build final book.bin
  const uint32_t buildStart = millis();
  {
    ProgressSpan buildSpan(reporter, 60, 80);
    if (!bookMetadataCache->buildBookBin(filepath, bookMetadata,
                                         [&buildSpan](const int percent) { buildSpan.reportPercent(percent); })) {
      LOG_ERR("EBP", "Could not update mappings and sizes");
      return false;
    }
    buildSpan.finish();
  }
  LOG_DBG("EBP", "buildBookBin completed in %lu ms", millis() - buildStart);
  LOG_HEAP("build:bookbin-done");
  LOG_DBG("EBP", "Total indexing completed in %lu ms", millis() - indexingStart);
  LOG_DBG("EBP", "spineCount=%d tocCount=%d", bookMetadataCache->getSpineCount(), bookMetadataCache->getTocCount());

  if (!bookMetadataCache->cleanupTmpFiles()) {
    LOG_DBG("EBP", "Could not cleanup tmp files - ignoring");
  }

  // Reload the cache from disk so it's in the correct state
  bookMetadataCache.reset(new BookMetadataCache(cachePath));
  {
    ProgressSpan reloadSpan(reporter, 80, 85);
    if (!bookMetadataCache->load([&reloadSpan](const int percent) { reloadSpan.reportPercent(percent); })) {
      LOG_ERR("EBP", "Failed to reload cache after writing");
      return false;
    }
    reloadSpan.finish();
  }

  if (!skipLoadingCss) {
    // Parse CSS files after cache reload
    ProgressSpan cssSpan(reporter, 85, 90);
    parseCssFiles();
    cssSpan.finish();
    LOG_HEAP("build:css-done");
  } else {
    reporter.report(90);
  }

  LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
  LOG_HEAP("load:done-built");
  return true;
}

bool Epub::ensureCssCache(const std::function<void(int)>& progressCallback) {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot prepare CSS cache before EPUB metadata is loaded");
    return false;
  }

  if (!cssParser) {
    cssParser.reset(new CssParser(cachePath));
  }

  if (cssParser->hasCache() && cssParser->loadFromCache()) {
    cssParser->clear();
    return true;
  }

  cssParser->deleteCache();
  cssFiles.clear();
  tocNcxItem.clear();
  tocNavItem.clear();

  if (!parseContentOpf(bookMetadataCache->coreMetadata)) {
    LOG_ERR("EBP", "Could not parse content.opf to prepare CSS cache");
    return false;
  }

  const auto previousProgressCallback = this->progressCallback;
  this->progressCallback = progressCallback;
  parseCssFiles();
  this->progressCallback = previousProgressCallback;
  return cssParser->hasCache();
}

bool Epub::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("EPB", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("EPB", "Failed to clear cache");
    return false;
  }

  LOG_DBG("EPB", "Cache cleared successfully");
  return true;
}

void Epub::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }

  Storage.mkdir(cachePath.c_str());
}

const std::string& Epub::getCachePath() const { return cachePath; }

const std::string& Epub::getPath() const { return filepath; }

const std::string& Epub::getTitle() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.title;
}

const std::string& Epub::getAuthor() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.author;
}

const std::string& Epub::getLanguage() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.language;
}

std::string Epub::getCoverBmpPath(bool cropped) const {
  const auto coverFileName = std::string("cover") + (cropped ? "_crop" : "");
  return cachePath + "/" + coverFileName + ".bmp";
}

bool Epub::generateCoverBmp(bool cropped) const {
  // Already generated, return true
  if (Storage.exists(getCoverBmpPath(cropped).c_str())) {
    return true;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot generate cover BMP, cache not loaded");
    return false;
  }

  const auto coverImageHref = bookMetadataCache->coreMetadata.coverItemHref;
  if (coverImageHref.empty()) {
    LOG_ERR("EBP", "No known cover image");
    return false;
  }

  if (coverImageHref.substr(coverImageHref.length() - 4) == ".jpg" ||
      coverImageHref.substr(coverImageHref.length() - 5) == ".jpeg") {
    LOG_DBG("EBP", "Generating BMP from JPG cover image (%s mode)", cropped ? "cropped" : "fit");
    const auto coverJpgTempPath = getCachePath() + "/.cover.jpg";

    FsFile coverJpg;
    if (!Storage.openFileForWrite("EBP", coverJpgTempPath, coverJpg)) {
      return false;
    }
    readItemContentsToStream(coverImageHref, coverJpg, 1024);
    coverJpg.close();

    if (!Storage.openFileForRead("EBP", coverJpgTempPath, coverJpg)) {
      return false;
    }

    FsFile coverBmp;
    if (!Storage.openFileForWrite("EBP", getCoverBmpPath(cropped), coverBmp)) {
      coverJpg.close();
      return false;
    }
    const bool success = JpegToBmpConverter::jpegFileToBmpStream(coverJpg, coverBmp, cropped);
    coverJpg.close();
    coverBmp.close();
    Storage.remove(coverJpgTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate BMP from cover image");
      Storage.remove(getCoverBmpPath(cropped).c_str());
    }
    LOG_DBG("EBP", "Generated BMP from JPG cover image, success: %s", success ? "yes" : "no");
    return success;
  }

  if (coverImageHref.substr(coverImageHref.length() - 4) == ".png") {
    LOG_DBG("EBP", "Generating BMP from PNG cover image (%s mode)", cropped ? "cropped" : "fit");
    const auto coverPngTempPath = getCachePath() + "/.cover.png";

    FsFile coverPng;
    if (!Storage.openFileForWrite("EBP", coverPngTempPath, coverPng)) {
      return false;
    }
    readItemContentsToStream(coverImageHref, coverPng, 1024);
    coverPng.close();

    if (!Storage.openFileForRead("EBP", coverPngTempPath, coverPng)) {
      return false;
    }

    FsFile coverBmp;
    if (!Storage.openFileForWrite("EBP", getCoverBmpPath(cropped), coverBmp)) {
      coverPng.close();
      return false;
    }
    const bool success = PngToBmpConverter::pngFileToBmpStream(coverPng, coverBmp, cropped);
    coverPng.close();
    coverBmp.close();
    Storage.remove(coverPngTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate BMP from PNG cover image");
      Storage.remove(getCoverBmpPath(cropped).c_str());
    }
    LOG_DBG("EBP", "Generated BMP from PNG cover image, success: %s", success ? "yes" : "no");
    return success;
  }

  LOG_ERR("EBP", "Cover image is not a supported format, skipping");
  return false;
}

std::string Epub::getThumbBmpPath() const { return cachePath + "/thumb_[HEIGHT].bmp"; }
std::string Epub::getThumbBmpPath(int height) const { return cachePath + "/thumb_" + std::to_string(height) + ".bmp"; }

bool Epub::generateThumbBmp(int height) const {
  // Already generated, return true
  if (Storage.exists(getThumbBmpPath(height).c_str())) {
    return true;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot generate thumb BMP, cache not loaded");
    return false;
  }

  const auto coverImageHref = bookMetadataCache->coreMetadata.coverItemHref;
  if (coverImageHref.empty()) {
    LOG_DBG("EBP", "No known cover image for thumbnail");
  } else if (coverImageHref.substr(coverImageHref.length() - 4) == ".jpg" ||
             coverImageHref.substr(coverImageHref.length() - 5) == ".jpeg") {
    LOG_DBG("EBP", "Generating thumb BMP from JPG cover image");
    const auto coverJpgTempPath = getCachePath() + "/.cover.jpg";

    FsFile coverJpg;
    if (!Storage.openFileForWrite("EBP", coverJpgTempPath, coverJpg)) {
      return false;
    }
    readItemContentsToStream(coverImageHref, coverJpg, 1024);
    coverJpg.close();

    if (!Storage.openFileForRead("EBP", coverJpgTempPath, coverJpg)) {
      return false;
    }

    FsFile thumbBmp;
    if (!Storage.openFileForWrite("EBP", getThumbBmpPath(height), thumbBmp)) {
      coverJpg.close();
      return false;
    }
    // Use smaller target size for Continue Reading card (half of screen: 240x400)
    // Generate 1-bit BMP for fast home screen rendering (no gray passes needed)
    int THUMB_TARGET_WIDTH = height * 0.6;
    int THUMB_TARGET_HEIGHT = height;
    const bool success = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(coverJpg, thumbBmp, THUMB_TARGET_WIDTH,
                                                                             THUMB_TARGET_HEIGHT);
    coverJpg.close();
    thumbBmp.close();
    Storage.remove(coverJpgTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate thumb BMP from JPG cover image");
      Storage.remove(getThumbBmpPath(height).c_str());
    }
    LOG_DBG("EBP", "Generated thumb BMP from JPG cover image, success: %s", success ? "yes" : "no");
    return success;
  } else if (coverImageHref.substr(coverImageHref.length() - 4) == ".png") {
    LOG_DBG("EBP", "Generating thumb BMP from PNG cover image");
    const auto coverPngTempPath = getCachePath() + "/.cover.png";

    FsFile coverPng;
    if (!Storage.openFileForWrite("EBP", coverPngTempPath, coverPng)) {
      return false;
    }
    readItemContentsToStream(coverImageHref, coverPng, 1024);
    coverPng.close();

    if (!Storage.openFileForRead("EBP", coverPngTempPath, coverPng)) {
      return false;
    }

    FsFile thumbBmp;
    if (!Storage.openFileForWrite("EBP", getThumbBmpPath(height), thumbBmp)) {
      coverPng.close();
      return false;
    }
    int THUMB_TARGET_WIDTH = height * 0.6;
    int THUMB_TARGET_HEIGHT = height;
    const bool success =
        PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(coverPng, thumbBmp, THUMB_TARGET_WIDTH, THUMB_TARGET_HEIGHT);
    coverPng.close();
    thumbBmp.close();
    Storage.remove(coverPngTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate thumb BMP from PNG cover image");
      Storage.remove(getThumbBmpPath(height).c_str());
    }
    LOG_DBG("EBP", "Generated thumb BMP from PNG cover image, success: %s", success ? "yes" : "no");
    return success;
  } else {
    LOG_ERR("EBP", "Cover image is not a supported format, skipping thumbnail");
  }

  // Write an empty bmp file to avoid generation attempts in the future
  FsFile thumbBmp;
  Storage.openFileForWrite("EBP", getThumbBmpPath(height), thumbBmp);
  thumbBmp.close();
  return false;
}

uint8_t* Epub::readItemContentsToBytes(const std::string& itemHref, size_t* size, const bool trailingNullByte) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return nullptr;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);

  const auto content = ZipFile(filepath).readFileToMemory(path.c_str(), size, trailingNullByte);
  if (!content) {
    LOG_DBG("EBP", "Failed to read item %s", path.c_str());
    return nullptr;
  }

  return content;
}

bool Epub::readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return false;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);
  return ZipFile(filepath).readFileToStream(path.c_str(), out, chunkSize);
}

bool Epub::getItemSize(const std::string& itemHref, size_t* size) const {
  const std::string path = FsHelpers::normalisePath(itemHref);
  return ZipFile(filepath).getInflatedFileSize(path.c_str(), size);
}

int Epub::getSpineItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }
  return bookMetadataCache->getSpineCount();
}

size_t Epub::getCumulativeSpineItemSize(const int spineIndex) const { return getSpineItem(spineIndex).cumulativeSize; }

BookMetadataCache::SpineEntry Epub::getSpineItem(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineItem called but cache not loaded");
    return {};
  }

  if (spineIndex < 0 || spineIndex >= bookMetadataCache->getSpineCount()) {
    LOG_ERR("EBP", "getSpineItem index:%d is out of range", spineIndex);
    return bookMetadataCache->getSpineEntry(0);
  }

  return bookMetadataCache->getSpineEntry(spineIndex);
}

BookMetadataCache::TocEntry Epub::getTocItem(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_DBG("EBP", "getTocItem called but cache not loaded");
    return {};
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_DBG("EBP", "getTocItem index:%d is out of range", tocIndex);
    return {};
  }

  return bookMetadataCache->getTocEntry(tocIndex);
}

int Epub::getTocItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }

  return bookMetadataCache->getTocCount();
}

// work out the section index for a toc index
int Epub::getSpineIndexForTocIndex(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex called but cache not loaded");
    return 0;
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex: tocIndex %d out of range", tocIndex);
    return 0;
  }

  const int spineIndex = bookMetadataCache->getTocEntry(tocIndex).spineIndex;
  if (spineIndex < 0) {
    LOG_DBG("EBP", "Section not found for TOC index %d", tocIndex);
    return 0;
  }

  return spineIndex;
}

int Epub::getTocIndexForSpineIndex(const int spineIndex) const { return getSpineItem(spineIndex).tocIndex; }

std::vector<int> Epub::getTocIndexesForSpineIndex(const int spineIndex) const {
  std::vector<int> tocIndexes;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getTocIndexesForSpineIndex called but cache not loaded");
    return tocIndexes;
  }

  const int tocCount = bookMetadataCache->getTocCount();
  for (int i = 0; i < tocCount; i++) {
    if (bookMetadataCache->getTocEntry(i).spineIndex == spineIndex) {
      tocIndexes.push_back(i);
    }
  }

  return tocIndexes;
}

int Epub::getTocIndexForSpineAndAnchor(const int spineIndex, const std::string& anchor) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getTocIndexForSpineAndAnchor called but cache not loaded");
    return -1;
  }

  const int fallbackIndex = getTocIndexForSpineIndex(spineIndex);
  if (anchor.empty()) {
    return fallbackIndex;
  }

  const int tocCount = bookMetadataCache->getTocCount();
  for (int i = 0; i < tocCount; i++) {
    const auto tocEntry = bookMetadataCache->getTocEntry(i);
    if (tocEntry.spineIndex == spineIndex && tocEntry.anchor == anchor) {
      return i;
    }
  }

  return fallbackIndex;
}

int Epub::getParentTocIndex(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getParentTocIndex called but cache not loaded");
    return -1;
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    return -1;
  }

  const auto current = bookMetadataCache->getTocEntry(tocIndex);
  if (current.level <= 1) {
    return -1;
  }

  for (int i = tocIndex - 1; i >= 0; i--) {
    const auto candidate = bookMetadataCache->getTocEntry(i);
    if (candidate.level < current.level) {
      return i;
    }
  }

  return -1;
}

std::string Epub::formatTocDisplayTitle(const int tocIndex) const {
  const auto current = getTocItem(tocIndex);
  if (current.title.empty()) {
    return "";
  }

  const int parentIndex = getParentTocIndex(tocIndex);
  if (parentIndex < 0) {
    return current.title;
  }

  const auto parent = getTocItem(parentIndex);
  if (parent.title.empty()) {
    return current.title;
  }

  return parent.title + ": " + current.title;
}

size_t Epub::getBookSize() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded() || bookMetadataCache->getSpineCount() == 0) {
    return 0;
  }
  return getCumulativeSpineItemSize(getSpineItemsCount() - 1);
}

int Epub::getSpineIndexForTextReference() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTextReference called but cache not loaded");
    return 0;
  }
  LOG_DBG("EBP", "Core Metadata: cover(%d)=%s, textReference(%d)=%s",
          bookMetadataCache->coreMetadata.coverItemHref.size(), bookMetadataCache->coreMetadata.coverItemHref.c_str(),
          bookMetadataCache->coreMetadata.textReferenceHref.size(),
          bookMetadataCache->coreMetadata.textReferenceHref.c_str());

  if (bookMetadataCache->coreMetadata.textReferenceHref.empty()) {
    // there was no textReference in epub, so we return 0 (the first chapter)
    return 0;
  }

  // loop through spine items to get the correct index matching the text href
  for (size_t i = 0; i < getSpineItemsCount(); i++) {
    if (getSpineItem(i).href == bookMetadataCache->coreMetadata.textReferenceHref) {
      LOG_DBG("EBP", "Text reference %s found at index %d", bookMetadataCache->coreMetadata.textReferenceHref.c_str(),
              i);
      return i;
    }
  }
  // This should not happen, as we checked for empty textReferenceHref earlier
  LOG_DBG("EBP", "Section not found for text reference");
  return 0;
}

// Calculate progress in book (returns 0.0-1.0)
float Epub::calculateProgress(const int currentSpineIndex, const float currentSpineRead) const {
  const size_t bookSize = getBookSize();
  if (bookSize == 0) {
    return 0.0f;
  }
  const size_t prevChapterSize = (currentSpineIndex >= 1) ? getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  const size_t curChapterSize = getCumulativeSpineItemSize(currentSpineIndex) - prevChapterSize;
  const float sectionProgSize = currentSpineRead * static_cast<float>(curChapterSize);
  const float totalProgress = static_cast<float>(prevChapterSize) + sectionProgSize;
  return totalProgress / static_cast<float>(bookSize);
}

int Epub::resolveHrefToSpineIndex(const std::string& href) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return -1;

  // Extract filename (remove #anchor)
  std::string target = href;
  size_t hashPos = target.find('#');
  if (hashPos != std::string::npos) target = target.substr(0, hashPos);

  // Same-file reference (anchor-only)
  if (target.empty()) return -1;

  // Extract just the filename for comparison
  size_t targetSlash = target.find_last_of('/');
  std::string targetFilename = (targetSlash != std::string::npos) ? target.substr(targetSlash + 1) : target;

  for (int i = 0; i < getSpineItemsCount(); i++) {
    const auto& spineHref = getSpineItem(i).href;
    // Try exact match first
    if (spineHref == target) return i;
    // Then filename-only match
    size_t spineSlash = spineHref.find_last_of('/');
    std::string spineFilename = (spineSlash != std::string::npos) ? spineHref.substr(spineSlash + 1) : spineHref;
    if (spineFilename == targetFilename) return i;
  }
  return -1;
}
