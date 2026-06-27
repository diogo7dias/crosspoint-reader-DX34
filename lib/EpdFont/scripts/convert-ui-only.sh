#!/bin/bash
# Narrow-scope conversion: regenerate ONLY the UI font bitmaps. Does NOT invoke
# convert-builtin-fonts.sh or touch any reader font.
#
# UI font = Pixel Operator (https://www.dafont.com/pixel-operator.font, CC0).
# Two sizes:
#
#   ui_16_regular  PixelOperator       @ 16px  -> status bar / hints (SMALL_FONT_ID)
#                                                 16 == native grid -> pixel-perfect.
#   ui_32_regular  PixelOperator       @ 32px  -> body, menus, buttons (UI_10_FONT_ID)
#   ui_32_bold     PixelOperator-Bold  @ 32px  -> selected rows / titles (BOLD style, same id)
#
# Grid note: BOTH sizes are exact integer multiples of Pixel Operator's 16px design
# grid (16 = 1x, 32 = 2x), so FreeType produces zero anti-aliasing — every design
# pixel maps to a whole 1x1 or 2x2 block and every stem is uniformly 1px / 2px. That
# is what makes the face look NEAT: off-grid sizes (e.g. 26/30px) land stems on
# fractional pixels, so even with a tuned --bw-threshold some stems end up 1px and
# others 2px (the visibly uneven 'M' legs). On-grid 32px has none of that — it is
# pixel-perfect, so --bw-threshold is moot (no grey pixels to threshold). Rendered
# with --dpi 72 so ppem == size. 1-bit, uncompressed.
#
# Titles (UI_12_FONT_ID) reuse the 32px face, distinguished by BOLD weight, and are
# aliased to UI_10_FONT_ID in fontIds.h.
set -e
cd "$(dirname "$0")"
PYTHON_BIN="${PYTHON_BIN:-python3}"

SRC="../builtinFonts/source/PixelOperator"
for f in PixelOperator.ttf PixelOperator-Bold.ttf; do
  if [[ ! -f "$SRC/$f" ]]; then
    echo "error: missing source font: $SRC/$f" >&2
    exit 1
  fi
done

gen() { # <out-header-name> <size> <ttf> <bw-threshold>
  echo "Generating $1 from $(basename "$3") @ ${2}px (threshold ${4})..."
  "$PYTHON_BIN" fontconvert.py "$1" "$2" "$SRC/$3" --dpi 72 --bw-threshold "$4" \
    > "../builtinFonts/$1.h"
}

gen ui_16_regular 16 PixelOperator.ttf       8   # status bar (1x native grid, crisp)
gen ui_32_regular 32 PixelOperator.ttf       8   # body / menus / titles (2x native grid, crisp)
gen ui_32_bold    32 PixelOperator-Bold.ttf  8   # bold weight for selection / titles

echo ""
echo "Generated 3 UI font headers (Pixel Operator: 16px status + 32px body regular/bold, both on-grid)."
