// DegradeLevel / DegradePlan — the layout degradation vocabulary (RFC #164).
//
// A monotone ladder the caller dials from a heap gate (MemoryPolicy's
// layoutLevel/renderLevel seam, RFC #163) and a resolved plan the LayoutEngine
// consumes as a pure value. Each level strictly sheds more than the prior, so
// "OOM -> restart" becomes "render in a simplified mode": first trim the glyph
// prewarm, then drop optimal (Knuth-Plass) line breaking for greedy, then drop
// hyphenation, then drop images. The engine never sees MemoryPolicy — it
// receives a DegradePlan value, so this header depends on nothing but <cstdint>
// and is fully host-compilable / host-testable.
//
// Step 1 (this header) is purely additive: no caller dials it yet, so device
// behaviour is unchanged until the engine and the policy seam are wired
// (RFC #164 steps 5-7).
#pragma once

#include <cstdint>

namespace crosspoint {
namespace layout {

// Glyph-style bits used by prewarmStyleMask. Mirrors the four EpdFontFamily
// styles the page tracks in usedStyleMask. Regular is always the cheapest to
// keep warm and the last to be shed.
enum StyleBit : uint8_t {
  kStyleRegular = 0x01,
  kStyleBold = 0x02,
  kStyleItalic = 0x04,
  kStyleBoldItalic = 0x08,
  kStyleAll = 0x0F,
};

enum class DegradeLevel : uint8_t {
  Full = 0,     // everything on; prewarm every used style
  TrimPrewarm,  // shed glyph prewarm to regular-only; layout still full fidelity
  SimpleBreak,  // greedy line breaking (drops the DP arrays) instead of optimal
  NoHyphen,     // also disable hyphenation / oversized-token splitting
  SkipImages,   // also skip image blocks — last resort to fit the heap
};

// Fully-resolved, per-section plan derived once from a level + the styles the
// page actually uses. Pure data; the engine reads the booleans/mask directly.
struct DegradePlan {
  bool images = true;          // render image blocks
  bool hyphenate = true;       // hyphenate / split oversized tokens at breaks
  bool optimalBreak = true;    // Knuth-Plass DP line breaking (false = greedy)
  uint8_t prewarmStyleMask = kStyleAll;  // which styles to warm in the glyph cache

  // Derive the plan for `level`, intersecting the prewarm mask with the styles
  // the page actually uses so we never warm a style that never appears. The
  // shedding is monotone: a higher level is a strict subset of a lower one
  // (no flag is ever re-enabled by going further down the ladder).
  static DegradePlan from(DegradeLevel level, uint8_t usedStyleMask) {
    DegradePlan p;
    const uint8_t used = static_cast<uint8_t>(usedStyleMask & kStyleAll);
    // Full keeps every used style warm; every degraded level trims prewarm to
    // regular-only (the cheapest, always-present style), intersected with what
    // the page uses.
    const uint8_t trimmed = static_cast<uint8_t>(used & kStyleRegular);

    switch (level) {
      case DegradeLevel::Full:
        p.prewarmStyleMask = used;
        break;
      case DegradeLevel::TrimPrewarm:
        p.prewarmStyleMask = trimmed;
        break;
      case DegradeLevel::SimpleBreak:
        p.prewarmStyleMask = trimmed;
        p.optimalBreak = false;
        break;
      case DegradeLevel::NoHyphen:
        p.prewarmStyleMask = trimmed;
        p.optimalBreak = false;
        p.hyphenate = false;
        break;
      case DegradeLevel::SkipImages:
        p.prewarmStyleMask = trimmed;
        p.optimalBreak = false;
        p.hyphenate = false;
        p.images = false;
        break;
    }
    return p;
  }
};

// ── Heap-pressure -> level mapping (RFC #164 step 7) ─────────────────────────
// Pure functions: largest-free-block bytes in, DegradeLevel out. The threshold
// NUMBERS are owned by MemoryPolicy (RFC #163, the `mem::k*Below*` constants)
// and passed in here, so this header stays dependency-free (<cstdint> only) and
// host-testable with synthetic thresholds. Monotone: a smaller largest block
// yields an equal-or-higher (more shedding) level.
//
// Tier A ladder (greedy SimpleBreak deferred — ParsedText has no greedy path
// yet, so the optimalBreak flag is inert): layout sheds hyphenation then images;
// render sheds glyph prewarm. The TrimPrewarm/SimpleBreak rungs are reachable
// only through DegradePlan::from for the render-mask side; layoutLevelFor jumps
// Full -> NoHyphen -> SkipImages because those are the levers layout honours.

// Layout-time level: governs hyphenation (its mid-vector inserts churn the heap)
// and image blocks (the largest single decode buffer). Precondition for sane
// banding: noHyphenBelow >= skipImagesBelow (drop the cheaper lever first).
constexpr DegradeLevel layoutLevelFor(size_t largestBytes, size_t noHyphenBelow, size_t skipImagesBelow) {
  if (largestBytes < skipImagesBelow) return DegradeLevel::SkipImages;
  if (largestBytes < noHyphenBelow) return DegradeLevel::NoHyphen;
  return DegradeLevel::Full;
}

// Render-time level: governs glyph prewarm warmth only. Below the threshold,
// warm regular-only instead of every used style, shrinking the simultaneous
// glyph-cache peak (the dominant render-OOM crowder per the v4.0.0 incident).
constexpr DegradeLevel renderLevelFor(size_t largestBytes, size_t trimPrewarmBelow) {
  return largestBytes < trimPrewarmBelow ? DegradeLevel::TrimPrewarm : DegradeLevel::Full;
}

}  // namespace layout
}  // namespace crosspoint
