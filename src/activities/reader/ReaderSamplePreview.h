#pragma once

class GfxRenderer;
class CrossPointSettings;

namespace reader {

// Renders the ~40-word appearance sample inside [boxX, boxY, boxW, boxH] using
// the given settings' reader appearance: reader font (getReaderFontId), line
// spacing, first-line indent (incl. Mega), paragraph alignment (justify / left /
// center / right), word spacing, paragraph spacing and render style (crisp /
// dark). The theme's horizontal margin is applied as a capped box inset (exact
// px margins do not translate into a scaled preview box). Text is clipped to the
// box height. The CALLER draws the surrounding box/border and MUST heap-gate the
// call (rendering the reader font decompresses its glyph group).
//
// Shared by the reader-settings live preview concept and the theme preview popup
// so a theme can be previewed exactly as it will read.
void drawReaderSamplePreview(GfxRenderer& renderer, const CrossPointSettings& s, int boxX, int boxY, int boxW,
                             int boxH);

}  // namespace reader
