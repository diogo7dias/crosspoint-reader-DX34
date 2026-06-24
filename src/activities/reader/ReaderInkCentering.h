#pragma once

// Shared vertical ink-centering policy for the reader activities.
//
// Both the EPUB (EpubReaderActivity::renderContents) and TXT
// (TxtReaderActivity::renderPage) render paths center full pages by the real
// glyph *ink* box rather than the nominal line boxes: the font ascender
// over-reserves space above caps that a normal first line never fills, and the
// line-spacing leading sits below the last line, so box-centering leaves
// visibly uneven top/bottom gaps. Each reader measures the ink box its own way
// (EPUB via Page::getInkBounds over laid-out blocks; TXT by scanning its
// FlowLine list with GfxRenderer::measureTextInk), but the final arithmetic —
// and its degenerate-input guards — were duplicated verbatim in both. That
// duplication is a mirror-fix hazard: a tweak to the centering formula had to
// land in two unrelated files or the readers silently diverged. This is the one
// shared, host-tested kernel.

namespace crosspoint {
namespace reader {

// Pixel offset to add to the content top margin so the glyph-ink box spanning
// [inkTop, inkBottom] (both measured relative to the content top, in pixels)
// sits vertically centered within a viewport of height vpHeight.
//
// Returns 0 (i.e. top-aligned, no shift) for degenerate inputs: a non-positive
// ink height, or an ink box taller than the viewport. Callers apply the result
// as `contentTop = marginTop + inkCenterOffset(...)`, so a 0 return preserves
// the previous top-aligned behaviour exactly.
constexpr int inkCenterOffset(int inkTop, int inkBottom, int vpHeight) {
  const int inkHeight = inkBottom - inkTop;
  if (inkHeight <= 0 || inkHeight > vpHeight) return 0;
  // Land the ink box centered: its top sits at (viewport slack) / 2, so the
  // content origin shifts up by inkTop to compensate for the empty ascender
  // band above the first line's real ink.
  return (vpHeight - inkHeight) / 2 - inkTop;
}

}  // namespace reader
}  // namespace crosspoint
