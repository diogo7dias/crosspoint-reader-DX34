// LayoutEngine — the EPUB per-paragraph layout module (RFC #164).
//
// This is the deep module that ChapterHtmlSlimParser feeds words into and that
// emits laid-out TextBlock lines. Its eventual job (RFC #164 steps 3-7) is to
// bound the per-paragraph working set to a pre-allocated LayoutArena and to
// render in a degraded mode under heap pressure instead of OOM->restart.
//
// Step 2 (this commit) is a behaviour-identical SHIM: the engine wraps the
// legacy `ParsedText` and forwards every call. The arena and DegradePlan are
// carried but DORMANT — the plan is pinned to Full, the arena is unused — so a
// healthy device lays out byte-for-byte as before. The point of this step is
// only to flip the parser onto the new contract (addWord -> bool,
// flush -> LayoutStatus) so steps 3-6 can replace the internals from the inside
// without touching the parser again.
//
// The engine is block-scoped for now (constructed per text block, exactly where
// `new ParsedText` used to be); step 6 hoists it to section scope with the
// arena pre-allocated once (repurposing the dead 24 KB layoutHeapAnchor_).
#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <memory>

#include "../ParsedText.h"
#include "../blocks/BlockStyle.h"
#include "../blocks/TextBlock.h"
#include "DegradeLevel.h"

class GfxRenderer;

namespace crosspoint {
namespace layout {

// The three honest outcomes of a flush, replacing ParsedText's single sticky
// hadOom() flag. ArenaOverflow is unreachable in the step-2 shim (no arena is
// driven yet); it becomes live when scratch/words move into the arena.
enum class LayoutStatus : uint8_t {
  Ok,             // laid out and emitted cleanly
  ArenaOverflow,  // ran out of bounded scratch mid-paragraph (steps 3-4+)
  EmitOom,        // a line/TextBlock heap allocation failed (the legacy hadOom)
};

class LayoutEngine {
 public:
  // Block-scoped ctor mirroring the ParsedText construction it replaces, plus
  // the renderer/fontId that ParsedText's layout call used to take per-flush
  // and a DegradePlan (pinned to Full by the parser until step 7).
  LayoutEngine(GfxRenderer& renderer, int fontId, bool extraParagraphSpacing, bool hyphenationEnabled,
               const BlockStyle& blockStyle, uint8_t wordSpacingPercent, uint8_t firstLineIndentMode,
               bool usePublisherStyles, DegradePlan plan = DegradePlan{})
      : renderer_(renderer),
        fontId_(fontId),
        plan_(plan),
        pt_(extraParagraphSpacing, hyphenationEnabled, blockStyle, wordSpacingPercent, firstLineIndentMode,
            usePublisherStyles) {}

  // Append a word. Returns false on EmitOom (the heap-probe inside the legacy
  // addWord bailed before a vector growth would have crashed) — the caller
  // stops feeding and routes the chapter to recovery. `len` is the byte length
  // (word need not be NUL-terminated, though the parser's buffer is).
  bool addWord(const char* word, size_t len, EpdFontFamily::Style fontStyle, bool underline = false,
               bool attachToPrevious = false) noexcept {
    pt_.addWord(std::string(word, len), fontStyle, underline, attachToPrevious);
    return !pt_.hadOom();
  }

  // Lay out the buffered words and emit each line via processLine. With
  // includeLastLine=false the trailing (possibly partial) line is left buffered
  // so the paragraph can keep accumulating — the 750-word drain relies on this.
  LayoutStatus flush(uint16_t viewportWidth, const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                     bool includeLastLine = true) noexcept {
    pt_.layoutAndExtractLines(renderer_, fontId_, viewportWidth, processLine, includeLastLine);
    return pt_.hadOom() ? LayoutStatus::EmitOom : LayoutStatus::Ok;
  }

  // — Accessors the parser still leans on (the leaky bits the RFC notes; later
  //   steps fold blockStyle/word-count handling into the engine so these
  //   shrink). —
  bool isEmpty() const { return pt_.isEmpty(); }
  size_t wordCount() const { return pt_.size(); }
  BlockStyle& blockStyle() { return pt_.getBlockStyle(); }
  void setBlockStyle(const BlockStyle& blockStyle) { pt_.setBlockStyle(blockStyle); }

 private:
  GfxRenderer& renderer_;
  int fontId_;
  DegradePlan plan_;  // dormant until step 7 (pinned Full)
  ParsedText pt_;     // the wrapped legacy layout, gutted in steps 3-4
};

}  // namespace layout
}  // namespace crosspoint
