#pragma once

class GfxRenderer;
class CrossPointSettings;

namespace reader {

// Renders the ~40-word appearance sample inside the content column
// [insetL, insetL + colW], flowing from y = top and clipped at y = bottom,
// using the given settings' reader appearance: reader font (getReaderFontId),
// line spacing, first-line indent (incl. Mega), paragraph alignment (justify /
// left / center / right), word spacing, paragraph spacing and render style
// (crisp / dark). The render style is saved and restored internally.
//
// The CALLER resolves the column geometry, because the two callers want
// different margin policies: the full-screen reader-settings preview passes the
// reader's real content column (ReaderLayoutSafety::resolveBaseReaderMargins) so
// margins read exactly as the page will; the theme preview popup passes a capped
// box inset (exact px margins do not translate into a scaled popup box). The
// caller also draws any surrounding box/border/separator and MUST heap-gate the
// call (rendering the reader font decompresses its glyph group).
//
// Single source of truth for the sample-text flow, shared by the reader-settings
// live preview and the theme preview popup so a theme reads exactly as saved.
void drawReaderSamplePreview(GfxRenderer& renderer, const CrossPointSettings& s, int insetL, int colW, int top,
                             int bottom);

}  // namespace reader
