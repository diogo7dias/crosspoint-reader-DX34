#pragma once

#include <expat.h>

#include <climits>
#include <functional>
#include <memory>
#include <vector>

#include "../FootnoteEntry.h"
#include "../layout/LayoutEngine.h"
#include "../page/PageBuilder.h"
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
  // RFC #164: the layout/render degradation plan for this section. Default Full
  // (everything on) — the activity dials it down from the heap gate in step 7.
  crosspoint::layout::DegradePlan degradePlan;
  // RFC #164 step 6: a section-lifetime arena the reader activity owns (the
  // repurposed 24 KB layout anchor), reused across sections. When set + ok, the
  // parser lays out into it instead of self-creating a per-section arena, so the
  // bounded word buffer is available even on a fragmented heap. nullptr keeps
  // the legacy self-create path (host tests, non-reader callers).
  crosspoint::layout::LayoutArena* externalArena = nullptr;
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
  // Bounded layout scratch (RFC #164 step 3), reserved once per section in
  // parseAndBuildPages and shared by every paragraph's LayoutEngine. Sized to
  // hold the word buffer + dp[]/ans[] line-break scratch for a dense paragraph.
  // The OWNED arena below is the legacy self-create path (only allocated when
  // the heap has comfortable headroom, else empty -> std::vector fallback). When
  // the reader activity passes a section-lifetime arena (externalArena_, the
  // repurposed 24 KB anchor, RFC #164 step 6) the parser uses that instead, so
  // the bounded buffer is available even on a fragmented heap. sectionArena_
  // points at whichever is active for the current parse.
  static constexpr size_t kLayoutScratchArenaBytes = 16 * 1024;
  crosspoint::layout::LayoutArena layoutArena_;                 // owned self-create fallback
  crosspoint::layout::LayoutArena* externalArena_ = nullptr;    // activity-owned arena (nullable), from config
  crosspoint::layout::LayoutArena* sectionArena_ = nullptr;     // active arena for this parse (set in parseAndBuildPages)
  std::unique_ptr<crosspoint::layout::LayoutEngine> currentTextBlock = nullptr;
  // RFC #171-followup: page assembly (in-flight page, vertical cursor,
  // completed-page count, pending anchors, complete-when-full + dense-fit) lives
  // in PageBuilder, which returns explicit [[nodiscard]] PageStatus instead of the
  // sticky parseFailed bool. Constructed in parseAndBuildPages once geometry is
  // known. The parser still owns expat parsing, style resolution, layout feed,
  // footnote tracking, and image *extraction* — it feeds PageBuilder laid-out
  // lines + already-built ImageBlocks.
  std::unique_ptr<crosspoint::page::PageBuilder> pageBuilder_ = nullptr;
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
  crosspoint::layout::DegradePlan degradePlan_;  // RFC #164: section degradation plan (Full until step 7)
  const CssParser* cssParser;
  std::string contentBase;
  std::string imageBasePath;
  int imageCounter = 0;

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
        degradePlan_(config.degradePlan),
        externalArena_(config.externalArena),
        completePageFn(completePageFn),
        anchorPageFn(anchorPageFn),
        progressFn(progressFn),
        cssParser(cssParser),
        contentBase(config.contentBase),
        imageBasePath(config.imageBasePath) {}

  ~ChapterHtmlSlimParser() = default;
  bool parseAndBuildPages();
};
