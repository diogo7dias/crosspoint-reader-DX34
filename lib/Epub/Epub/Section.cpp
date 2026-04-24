#include "Section.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <esp_task_wdt.h>

#include <algorithm>

#include "Page.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint8_t SECTION_FILE_VERSION = 21;
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(uint8_t) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint8_t) + sizeof(bool) + sizeof(uint16_t) +
                                 sizeof(uint32_t);

int getAnchorPage(const std::vector<std::pair<std::string, uint16_t>>& anchors, const std::string& anchor) {
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

std::vector<int16_t> buildPageTocLut(const std::shared_ptr<Epub>& epub, const int spineIndex, const uint16_t pageCount,
                                     const std::vector<std::pair<std::string, uint16_t>>& anchors) {
  std::vector<int16_t> pageTocLut(pageCount, -1);
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
                                     const uint8_t textRenderMode, const bool readerBoldSwap) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(fontId) + sizeof(lineCompression) +
                                   sizeof(extraParagraphSpacingLevel) + sizeof(paragraphAlignment) +
                                   sizeof(viewportWidth) + sizeof(viewportHeight) + sizeof(pageCount) +
                                   sizeof(hyphenationEnabled) + sizeof(wordSpacingPercent) +
                                   sizeof(firstLineIndentMode) + sizeof(readerStyleMode) + sizeof(textRenderMode) +
                                   sizeof(readerBoldSwap) + sizeof(uint32_t),
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
  serialization::writePod(file, pageCount);  // Placeholder for page count (will be initially 0 when written)
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

  // Pre-check heap: pageLut is pageCount * sizeof(uint32_t)
  {
    const size_t needed = pageCount * sizeof(uint32_t);
    void* check = malloc(needed);
    if (!check) {
      file.close();
      LOG_ERR("SCT", "Deserialization OOM: pageLut needs %u bytes", (unsigned)needed);
      clearCache();
      return false;
    }
    free(check);
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
    anchorLut.reserve(anchorCount);
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
                                const std::function<void(int)>& progressFn) {
  LOG_DBG("HEAP", "SCT createSectionFile:start spine=%d free=%u largest=%u min=%u", spineIndex,
          (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
          (unsigned)ESP.getMinFreeHeap());
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
    LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
    return false;
  }

  LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes)", tmpHtmlPath.c_str(), fileSize);

  if (!Storage.openFileForWrite("SCT", filePath, file)) {
    return false;
  }
  writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacingLevel, paragraphAlignment, viewportWidth,
                         viewportHeight, hyphenationEnabled, wordSpacingPercent, firstLineIndentMode, readerStyleMode,
                         textRenderMode, readerBoldSwap);
  std::vector<uint32_t> lut = {};
  std::vector<std::pair<std::string, uint16_t>> anchors = {};

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  CssParser* cssParser = nullptr;
  if (readerStyleMode != 0) {
    cssParser = epub->getCssParser();
    if (cssParser) {
      if (!cssParser->loadFromCache()) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  ChapterHtmlSlimParser visitor(
      epub, tmpHtmlPath, renderer, fontId, lineCompression, extraParagraphSpacingLevel, paragraphAlignment,
      viewportWidth, viewportHeight, hyphenationEnabled, wordSpacingPercent, firstLineIndentMode, readerStyleMode != 0,
      [this, &lut](std::unique_ptr<Page> page) { lut.emplace_back(this->onPageComplete(std::move(page))); },
      [&anchors](const std::string& anchor, const uint16_t pageIndex) { anchors.emplace_back(anchor, pageIndex); },
      contentBase, imageBasePath, progressFn, cssParser);
  success = visitor.parseAndBuildPages();

  Storage.remove(tmpHtmlPath.c_str());
  if (!success) {
    LOG_ERR("SCT", "Failed to parse XML and build pages");
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
    LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
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
