#pragma once

#include <expat.h>

#include <climits>
#include <functional>
#include <memory>
#include <vector>

#include "../FootnoteEntry.h"
#include "../ParsedText.h"
#include "FootnotePlacer.h"
#include "../blocks/ImageBlock.h"
#include "../blocks/TextBlock.h"
#include "../css/CssParser.h"
#include "../css/CssStyle.h"
#include "StyleResolver.h"

class Page;
class GfxRenderer;
class Epub;

#define MAX_WORD_SIZE 200

// Layout knobs for a chapter parse, bundled to collapse the former 18-parameter
// constructor (RFC #170). Collaborators (epub/filepath/renderer/callbacks/
// cssParser) remain explicit ctor params; these are the per-render settings.
struct ChapterParseConfig {
  int fontId = 0;
  float lineCompression = 1.0f;
  uint8_t extraParagraphSpacingLevel = 0;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  bool hyphenationEnabled = false;
  uint8_t wordSpacingPercent = 1;
  uint8_t firstLineIndentMode = 0;
  bool usePublisherStyles = true;
  std::string contentBase;
  std::string imageBasePath;
};

class ChapterHtmlSlimParser {
  std::shared_ptr<Epub> epub;
  const std::string& filepath;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>)> completePageFn;
  std::function<void(const std::string&, uint16_t)> anchorPageFn;
  std::function<void(int)> progressFn;  // Progress callback
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  bool nextWordContinues = false;  // true when next flushed word attaches to previous (inline element boundary)
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  // Sticky OOM flag. Set when a `new (nothrow) Page()` returns null
  // inside one of the parse callbacks, where we can't propagate a
  // return value through expat. The parse loop polls this between
  // XML_ParseBuffer calls and bails to the cleanup-and-return-false
  // branch as if the buffer alloc itself had failed. Without this, the
  // pre-PR-#97 code used bare `new Page()` and let bad_alloc panic the
  // ESP32 (no exceptions enabled in firmware build).
  bool parseFailed = false;
  int fontId;
  float lineCompression;
  uint8_t extraParagraphSpacingLevel;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  uint8_t wordSpacingPercent;
  uint8_t firstLineIndentMode;
  bool usePublisherStyles;
  const CssParser* cssParser;
  std::string contentBase;
  std::string imageBasePath;
  int imageCounter = 0;
  uint16_t completedPageCount = 0;
  std::vector<std::string> pendingAnchors;

  // Style tracking: the three former systems (depth flags + inline stack +
  // block CSS base) now live behind one host-testable resolver (RFC #170).
  StyleResolver styleResolver_;

  // Footnote link tracking. The word-index -> page assignment (pending queue +
  // cumulative word counter) now lives in a host-testable FootnotePlacer (RFC #170).
  bool insideFootnoteLink = false;
  int footnoteLinkDepth = -1;
  FootnoteEntry currentFootnote = {};
  int currentFootnoteLinkTextLen = 0;
  FootnotePlacer footnotePlacer_;

  void startNewTextBlock(const BlockStyle& blockStyle);
  void flushPartWordBuffer();
  void completeCurrentPage();
  void bindPendingAnchorsToCurrentPage();
  void makePages();
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  explicit ChapterHtmlSlimParser(std::shared_ptr<Epub> epub, const std::string& filepath, GfxRenderer& renderer,
                                 const ChapterParseConfig& config,
                                 const std::function<void(std::unique_ptr<Page>)>& completePageFn,
                                 const std::function<void(const std::string&, uint16_t)>& anchorPageFn,
                                 const std::function<void(int)>& progressFn = nullptr,
                                 const CssParser* cssParser = nullptr)

      : epub(epub),
        filepath(filepath),
        renderer(renderer),
        fontId(config.fontId),
        lineCompression(config.lineCompression),
        extraParagraphSpacingLevel(config.extraParagraphSpacingLevel),
        paragraphAlignment(config.paragraphAlignment),
        viewportWidth(config.viewportWidth),
        viewportHeight(config.viewportHeight),
        hyphenationEnabled(config.hyphenationEnabled),
        wordSpacingPercent(config.wordSpacingPercent),
        firstLineIndentMode(config.firstLineIndentMode),
        usePublisherStyles(config.usePublisherStyles),
        completePageFn(completePageFn),
        anchorPageFn(anchorPageFn),
        progressFn(progressFn),
        cssParser(cssParser),
        contentBase(config.contentBase),
        imageBasePath(config.imageBasePath) {}

  ~ChapterHtmlSlimParser() = default;
  bool parseAndBuildPages();
  void addLineToPage(std::shared_ptr<TextBlock> line);
};
