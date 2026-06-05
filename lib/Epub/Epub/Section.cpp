#include "Section.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MemoryPolicy.h>
#include <Serialization.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <deque>

#include "Page.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
// v22 (2026-04-24): forces every pre-existing .sct to re-layout. v21 caches
// that pre-date the CustomFontGlyphSource metrics-fallback have zero widths
// baked in for custom-font pages — layout ran while the font cache was
// released and lookup() returned nullptr for every codepoint, collapsing
// word positions. Loading a v21 cache on that path renders stacked/
// overlapping text on the first visible page. Bumping the version
// invalidates the bad caches on first open after flash; users lose at most
// ~80 s per previously-built chapter to re-layout, and book reading
// position (progress.bin) is unaffected.
//
// v23 (2026-06-04, RFC #164 step 7): header gains a degradeLevel byte and
// degraded layouts (NoHyphen/SkipImages, produced only under heap pressure)
// are never trusted on load — they re-layout on every open so the section
// snaps back to Full once the heap recovers. Bumping invalidates v22 caches so
// the new byte is present from the first open after flash.
constexpr uint8_t SECTION_FILE_VERSION = 23;
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(uint8_t) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint8_t) + sizeof(bool) +
                                 sizeof(uint8_t) /* degradeLevel */ + sizeof(uint16_t) + sizeof(uint32_t);

int getAnchorPage(const std::deque<std::pair<std::string, uint16_t>>& anchors, const std::string& anchor) {
  if (anchor.empty()) {
    return -1;
  }

  for (const auto& entry : anchors) {
    if (entry.first == anchor) {
      return entry.second;
    }
  }

  return -1;
}

std::deque<int16_t> buildPageTocLut(const std::shared_ptr<Epub>& epub, const int spineIndex, const uint16_t pageCount,
                                    const std::deque<std::pair<std::string, uint16_t>>& anchors) {
  std::deque<int16_t> pageTocLut(pageCount, -1);
  if (!epub || pageCount == 0) {
    return pageTocLut;
  }

  const int fallbackIndex = epub->getTocIndexForSpineIndex(spineIndex);
  struct TocTransition {
    int pageIndex = -1;
    int tocIndex = -1;
    int order = -1;
  };
  std::vector<TocTransition> transitions;

  const auto tocIndexes = epub->getTocIndexesForSpineIndex(spineIndex);
  int order = 0;
  for (const int tocIndex : tocIndexes) {
    const auto tocItem = epub->getTocItem(tocIndex);
    if (tocItem.spineIndex != spineIndex || tocItem.anchor.empty()) {
      continue;
    }

    const int pageIndex = getAnchorPage(anchors, tocItem.anchor);
    if (pageIndex < 0 || pageIndex >= pageCount) {
      continue;
    }

    transitions.push_back(TocTransition{.pageIndex = pageIndex, .tocIndex = tocIndex, .order = order++});
  }

  std::stable_sort(transitions.begin(), transitions.end(), [](const TocTransition& lhs, const TocTransition& rhs) {
    if (lhs.pageIndex != rhs.pageIndex) {
      return lhs.pageIndex < rhs.pageIndex;
    }
    return lhs.order < rhs.order;
  });

  int activeTocIndex = fallbackIndex;
  size_t transitionIndex = 0;
  for (uint16_t pageIndex = 0; pageIndex < pageCount; pageIndex++) {
    while (transitionIndex < transitions.size() && transitions[transitionIndex].pageIndex <= pageIndex) {
      activeTocIndex = transitions[transitionIndex].tocIndex;
      transitionIndex++;
    }
    pageTocLut[pageIndex] = static_cast<int16_t>(activeTocIndex);
  }

  return pageTocLut;
}
}  // namespace

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed", pageCount);

  pageCount++;
  return position;
}

void Section::writeSectionFileHeader(const int fontId, const float lineCompression,
                                     const uint8_t extraParagraphSpacingLevel, const uint8_t paragraphAlignment,
                                     const uint16_t viewportWidth, const uint16_t viewportHeight,
                                     const bool hyphenationEnabled, const uint8_t wordSpacingPercent,
                                     const uint8_t firstLineIndentMode, const uint8_t readerStyleMode,
                                     const uint8_t textRenderMode, const bool readerBoldSwap,
                                     const uint8_t degradeLevel) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(fontId) + sizeof(lineCompression) +
                                   sizeof(extraParagraphSpacingLevel) + sizeof(paragraphAlignment) +
                                   sizeof(viewportWidth) + sizeof(viewportHeight) + sizeof(pageCount) +
                                   sizeof(hyphenationEnabled) + sizeof(wordSpacingPercent) +
                                   sizeof(firstLineIndentMode) + sizeof(readerStyleMode) + sizeof(textRenderMode) +
                                   sizeof(readerBoldSwap) + sizeof(degradeLevel) + sizeof(uint32_t),
                "Header size mismatch");
  serialization::writePod(file, SECTION_FILE_VERSION);
  serialization::writePod(file, fontId);
  serialization::writePod(file, lineCompression);
  serialization::writePod(file, extraParagraphSpacingLevel);
  serialization::writePod(file, paragraphAlignment);
  serialization::writePod(file, viewportWidth);
  serialization::writePod(file, viewportHeight);
  serialization::writePod(file, hyphenationEnabled);
  serialization::writePod(file, wordSpacingPercent);
  serialization::writePod(file, firstLineIndentMode);
  serialization::writePod(file, readerStyleMode);
  serialization::writePod(file, textRenderMode);
  serialization::writePod(file, readerBoldSwap);
  serialization::writePod(file, degradeLevel);  // RFC #164 step 7: which DegradeLevel produced this layout
  serialization::writePod(file, pageCount);     // Placeholder for page count (will be initially 0 when written)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for LUT offset
}

bool Section::loadSectionFile(const int fontId, const float lineCompression, const uint8_t extraParagraphSpacingLevel,
                              const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, const bool hyphenationEnabled,
                              const uint8_t wordSpacingPercent, const uint8_t firstLineIndentMode,
                              const uint8_t readerStyleMode, const uint8_t textRenderMode, const bool readerBoldSwap) {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  // Match parameters
  {
    uint8_t version;
    serialization::readPod(file, version);
    if (version != SECTION_FILE_VERSION) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
      clearCache();
      return false;
    }

    int fileFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    uint8_t fileExtraParagraphSpacingLevel;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    uint8_t fileWordSpacingPercent;
    uint8_t fileFirstLineIndentMode;
    uint8_t fileReaderStyleMode;
    uint8_t fileTextRenderMode;
    bool fileReaderBoldSwap;
    uint8_t fileDegradeLevel;
    serialization::readPod(file, fileFontId);
    serialization::readPod(file, fileLineCompression);
    serialization::readPod(file, fileExtraParagraphSpacingLevel);
    serialization::readPod(file, fileParagraphAlignment);
    serialization::readPod(file, fileViewportWidth);
    serialization::readPod(file, fileViewportHeight);
    serialization::readPod(file, fileHyphenationEnabled);
    serialization::readPod(file, fileWordSpacingPercent);
    serialization::readPod(file, fileFirstLineIndentMode);
    serialization::readPod(file, fileReaderStyleMode);
    serialization::readPod(file, fileTextRenderMode);
    serialization::readPod(file, fileReaderBoldSwap);
    serialization::readPod(file, fileDegradeLevel);

    // RFC #164 step 7: a degraded cache (laid out under heap pressure with
    // hyphenation and/or images shed) is never trusted — reject it so the
    // section re-lays-out, snapping back to Full once the heap has recovered.
    // On a healthy device every cache is Full and this is a no-op. The book's
    // reading position (progress.bin) is unaffected by the re-layout.
    if (fileDegradeLevel != static_cast<uint8_t>(crosspoint::layout::DegradeLevel::Full)) {
      file.close();
      LOG_DIAG("SCT", "reject degraded cache spine=%d level=%u -> re-layout", spineIndex, (unsigned)fileDegradeLevel);
      clearCache();
      return false;
    }

    if (fontId != fileFontId || lineCompression != fileLineCompression ||
        extraParagraphSpacingLevel != fileExtraParagraphSpacingLevel || paragraphAlignment != fileParagraphAlignment ||
        viewportWidth != fileViewportWidth || viewportHeight != fileViewportHeight ||
        hyphenationEnabled != fileHyphenationEnabled || wordSpacingPercent != fileWordSpacingPercent ||
        firstLineIndentMode != fileFirstLineIndentMode || readerStyleMode != fileReaderStyleMode ||
        textRenderMode != fileTextRenderMode || readerBoldSwap != fileReaderBoldSwap) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
      clearCache();
      return false;
    }
  }

  serialization::readPod(file, pageCount);
  if (pageCount > 5000) {
    file.close();
    LOG_ERR("SCT", "Deserialization failed: page count %u exceeds maximum", pageCount);
    clearCache();
    return false;
  }
  uint32_t lutOffset;
  file.seek(HEADER_SIZE - sizeof(uint32_t));
  serialization::readPod(file, lutOffset);

  // Pre-check heap budget: pageLut is pageCount * sizeof(uint32_t).
  // pageLut is a std::deque, which allocates in small fixed-size chunks
  // rather than a single contiguous block, so we check total free heap
  // (not largest contiguous block) before deserializing. Deserialization
  // would otherwise abort mid-loop from per-chunk new() failure.
  {
    const size_t needed = pageCount * sizeof(uint32_t);
    const size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (freeHeap < needed) {
      file.close();
      LOG_ERR("SCT", "Deserialization OOM: pageLut needs %u bytes, free=%u", (unsigned)needed, (unsigned)freeHeap);
      clearCache();
      return false;
    }
  }
  pageLut.resize(pageCount);
  anchorLut.clear();
  pageTocLut.clear();
  file.seek(lutOffset);
  for (uint16_t i = 0; i < pageCount; i++) {
    serialization::readPod(file, pageLut[i]);
  }
  if (file.position() < file.size()) {
    uint16_t anchorCount = 0;
    serialization::readPod(file, anchorCount);
    // No reserve() on std::deque — but per-chunk allocation is small enough
    // that growth never triggers a large contiguous request.
    for (uint16_t i = 0; i < anchorCount; i++) {
      std::string anchor;
      uint16_t pageIndex = 0;
      serialization::readString(file, anchor);
      serialization::readPod(file, pageIndex);
      anchorLut.emplace_back(std::move(anchor), pageIndex);
    }
  }
  if (file.position() < file.size()) {
    uint16_t pageTocCount = 0;
    serialization::readPod(file, pageTocCount);
    pageTocLut.resize(pageTocCount);
    for (uint16_t i = 0; i < pageTocCount; i++) {
      serialization::readPod(file, pageTocLut[i]);
    }
  }
  file.close();
  LOG_DBG("SCT", "Deserialization succeeded: %d pages", pageCount);
  return true;
}

// static
size_t Section::pruneStaleCachesForFont(const std::string& cachePath, int currentFontId) {
  const std::string sectionsDir = cachePath + "/sections";
  auto dir = Storage.open(sectionsDir.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }
  size_t removed = 0;
  size_t iterations = 0;
  char entryName[256];
  for (auto e = dir.openNextFile(); e; e = dir.openNextFile()) {
    // Each iteration does up to one openFileForRead + one remove, both
    // ~ms on slow SD. Long sections/ dirs (or churned histories) could
    // approach the 5 s task watchdog without periodic feeding.
    if ((++iterations & 0x1F) == 0) esp_task_wdt_reset();
    e.getName(entryName, sizeof(entryName));
    const bool isDir = e.isDirectory();
    e.close();
    if (isDir) continue;
    const size_t nameLen = strlen(entryName);
    if (nameLen < 5 || strcmp(entryName + nameLen - 4, ".bin") != 0) continue;
    const std::string fullPath = sectionsDir + "/" + entryName;
    FsFile peek;
    if (!Storage.openFileForRead("SCT", fullPath, peek)) continue;
    uint8_t version = 0;
    int storedFontId = 0;
    bool ok = false;
    if (peek.read(&version, sizeof(version)) == sizeof(version) &&
        peek.read(reinterpret_cast<uint8_t*>(&storedFontId), sizeof(storedFontId)) == sizeof(storedFontId)) {
      ok = true;
    }
    peek.close();
    if (!ok) continue;
    if (storedFontId != currentFontId) {
      if (Storage.remove(fullPath.c_str())) {
        ++removed;
      }
    }
  }
  dir.close();
  if (removed > 0) {
    LOG_DBG("SCT", "pruneStaleCachesForFont: removed %u stale files (fontId now %d)", (unsigned)removed, currentFontId);
  }
  return removed;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  if (!Storage.exists(filePath.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

bool Section::createSectionFile(const int fontId, const float lineCompression, const uint8_t extraParagraphSpacingLevel,
                                const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                const uint16_t viewportHeight, const bool hyphenationEnabled,
                                const uint8_t wordSpacingPercent, const uint8_t firstLineIndentMode,
                                const uint8_t readerStyleMode, const uint8_t textRenderMode, const bool readerBoldSwap,
                                const std::function<void(int)>& progressFn,
                                crosspoint::layout::LayoutArena* layoutArena) {
  {
    const unsigned freeHeap = ESP.getFreeHeap();
    const unsigned largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const unsigned minHeap = ESP.getMinFreeHeap();
    LOG_DBG("HEAP", "SCT createSectionFile:start spine=%d free=%u largest=%u min=%u", spineIndex, freeHeap,
            largestBlock, minHeap);
    // Snapshot to crash_report.txt only when the heap is already
    // suspiciously fragmented at the start of layout — keeps the 16-line
    // RTC ring usable for the actual failure breadcrumb instead of crowding
    // it with healthy-open noise.
    if (largestBlock < 64 * 1024) {
      LOG_DIAG("SCT", "createSectionFile:start (low heap) spine=%d fontId=%d free=%u largest=%u min=%u", spineIndex,
               fontId, freeHeap, largestBlock, minHeap);
    }
  }
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // Free CSS rules to reclaim heap for ZIP decompression (needs ~44 KB
  // contiguous).  Rules reload from cache at the layout step below.
  {
    CssParser* css = epub->getCssParser();
    if (css) css->clear();
  }

  // Retry logic for SD card timing issues
  bool success = false;
  uint32_t fileSize = 0;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    esp_task_wdt_reset();
    if (attempt > 0) {
      LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
      delay(50);  // Brief delay before retry
    }

    // Remove any incomplete file from previous attempt before retrying
    if (Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }

    FsFile tmpHtml;
    if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
      continue;
    }
    success = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
    fileSize = tmpHtml.size();
    tmpHtml.close();

    // If streaming failed, remove the incomplete file immediately
    if (!success && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
      LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
    }
  }

  if (!success) {
    LOG_DIAG("SCT", "stream extract fail spine=%d free=%u largest=%u min=%u", spineIndex, (unsigned)ESP.getFreeHeap(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap());
    return false;
  }

  LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes)", tmpHtmlPath.c_str(), fileSize);

  if (!Storage.openFileForWrite("SCT", filePath, file)) {
    LOG_DIAG("SCT", "open section file fail path=%s free=%u largest=%u", filePath.c_str(), (unsigned)ESP.getFreeHeap(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return false;
  }
  // RFC #164 step 7: pick a layout degradation level from the heap as it stands
  // right before layout. On a healthy device this is Full (>=48 KB largest) and
  // the pipeline behaves byte-identically to before; only a heap that has been
  // squeezed down to the recovery-ladder band degrades (drop hyphenation, then
  // images) instead of OOM->restart. The chosen level is stamped in the header
  // so a degraded cache re-lays-out once the heap recovers (loadSectionFile
  // rejects any non-Full cache). DegradePlan::from resolves the level into the
  // images/hyphenate levers the parser honours.
  const crosspoint::layout::DegradeLevel layoutLevel = crosspoint::layout::layoutLevelFor(
      crosspoint::heap::largestFreeBlockBytes(), crosspoint::mem::kLayoutNoHyphenBelowBytes,
      crosspoint::mem::kLayoutSkipImagesBelowBytes);
  if (layoutLevel != crosspoint::layout::DegradeLevel::Full) {
    LOG_DIAG("SCT", "layout degraded spine=%d level=%u largest=%u", spineIndex, (unsigned)layoutLevel,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  }
  writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacingLevel, paragraphAlignment, viewportWidth,
                         viewportHeight, hyphenationEnabled, wordSpacingPercent, firstLineIndentMode, readerStyleMode,
                         textRenderMode, readerBoldSwap, static_cast<uint8_t>(layoutLevel));
  // Deques instead of vectors so growth doesn't require a contiguous realloc
  // when the chapter has many anchors / pages. See member-deque rationale
  // in Section.h. Crash 1 in the 2026-05-17 v2.3.9 test session was a
  // vector growth of the anchors pair-of-string list (14 KB request, 6 KB
  // largest contiguous) → std::bad_alloc → abort.
  std::deque<uint32_t> lut = {};
  std::deque<std::pair<std::string, uint16_t>> anchors = {};

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  // The visitor's ctor only stashes the cssParser pointer; it isn't
  // dereferenced until parseAndBuildPages() walks element callbacks. So
  // we can construct the visitor first and only reload the CSS rules
  // dictionary (~100 KB) immediately before the parser starts traversing.
  // Doing it this way flattens the heap-peak: previously the CSS reload
  // happened before the parser was constructed, holding both peaks
  // simultaneously and crowding out other allocations.
  CssParser* cssParser = (readerStyleMode != 0) ? epub->getCssParser() : nullptr;
  ChapterParseConfig parseConfig{
      fontId,         lineCompression,    extraParagraphSpacingLevel, paragraphAlignment,  viewportWidth,
      viewportHeight, hyphenationEnabled, wordSpacingPercent,         firstLineIndentMode, readerStyleMode != 0,
      contentBase,    imageBasePath};
  // RFC #164 step 6: hand the parser the reader activity's section-lifetime
  // arena (the repurposed 24 KB anchor) when provided, so the bounded word
  // buffer is available even on a fragmented heap.
  parseConfig.externalArena = layoutArena;
  // RFC #164 step 7: resolve the level into the per-section degradation plan the
  // parser reads (images branch + hyphenation). prewarmStyleMask is unused at
  // layout time (it gates render-side glyph warmth); kStyleAll is a no-op here.
  parseConfig.degradePlan = crosspoint::layout::DegradePlan::from(layoutLevel, crosspoint::layout::kStyleAll);
  ChapterHtmlSlimParser visitor(
      epub, tmpHtmlPath, renderer, parseConfig,
      [this, &lut](std::unique_ptr<Page> page) { lut.emplace_back(this->onPageComplete(std::move(page))); },
      [&anchors](const std::string& anchor, const uint16_t pageIndex) { anchors.emplace_back(anchor, pageIndex); },
      progressFn, cssParser);

  if (cssParser) {
    if (!cssParser->loadFromCache()) {
      LOG_ERR("SCT", "Failed to load CSS from cache");
    }
  }

  success = visitor.parseAndBuildPages();

  Storage.remove(tmpHtmlPath.c_str());
  if (!success) {
    LOG_DIAG("SCT", "parseAndBuildPages fail spine=%d fontId=%d free=%u largest=%u min=%u", spineIndex, fontId,
             (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)ESP.getMinFreeHeap());
    file.close();
    Storage.remove(filePath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }

  const uint32_t lutOffset = file.position();
  bool hasFailedLutRecords = false;
  // Write LUT
  for (const uint32_t& pos : lut) {
    if (pos == 0) {
      hasFailedLutRecords = true;
      break;
    }
    serialization::writePod(file, pos);
  }

  serialization::writePod(file, static_cast<uint16_t>(anchors.size()));
  for (const auto& entry : anchors) {
    serialization::writeString(file, entry.first);
    serialization::writePod(file, entry.second);
  }

  pageTocLut = buildPageTocLut(epub, spineIndex, pageCount, anchors);
  serialization::writePod(file, static_cast<uint16_t>(pageTocLut.size()));
  for (const int16_t tocIndex : pageTocLut) {
    serialization::writePod(file, tocIndex);
  }

  if (hasFailedLutRecords) {
    LOG_DIAG("SCT", "LUT write fail (invalid page positions) spine=%d pageCount=%u free=%u largest=%u", spineIndex,
             (unsigned)pageCount, (unsigned)ESP.getFreeHeap(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    file.close();
    Storage.remove(filePath.c_str());
    return false;
  }

  // Go back and write LUT offset
  file.seek(HEADER_SIZE - sizeof(uint32_t) - sizeof(pageCount));
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  pageLut = lut;
  anchorLut = std::move(anchors);
  file.close();
  if (cssParser) {
    cssParser->clear();
  }
  return true;
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() { return loadPageFromSectionFile(currentPage); }

std::unique_ptr<Page> Section::loadPageFromSectionFile(const int pageIndex) {
  if (pageIndex < 0 || static_cast<size_t>(pageIndex) >= pageLut.size()) {
    return nullptr;
  }
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return nullptr;
  }

  file.seek(pageLut[pageIndex]);
  auto page = Page::deserialize(file);
  file.close();
  return page;
}

int Section::getPageForAnchor(const std::string& anchor) const {
  if (anchor.empty()) {
    return -1;
  }

  for (const auto& entry : anchorLut) {
    if (entry.first == anchor) {
      return entry.second;
    }
  }

  return -1;
}

std::string Section::getCurrentAnchorForPage(const int page) const {
  if (page < 0 || anchorLut.empty()) {
    return "";
  }

  std::string bestAnchor;
  int bestPage = -1;
  for (const auto& entry : anchorLut) {
    if (entry.second <= page && entry.second >= bestPage) {
      bestPage = entry.second;
      bestAnchor = entry.first;
    }
  }

  return bestAnchor;
}

int Section::getTocIndexForPage(const int page) const {
  if (page < 0 || static_cast<size_t>(page) >= pageTocLut.size()) {
    return epub ? epub->getTocIndexForSpineIndex(spineIndex) : -1;
  }

  return pageTocLut[page];
}
