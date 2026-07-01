#!/bin/bash
# Narrow-scope conversion: regenerate ONLY the UI font bitmaps. Does NOT invoke
# convert-builtin-fonts.sh or touch any reader font.
#
# UI font = Cozette (https://github.com/slavfox/Cozette, MIT), vector build.
# Lector restores Cozette as the UI face (mom's DX34 v11.0.0 look) in place of
# Pixel Operator. Three distinct sizes at v11's ppem (dpi 150 default -> ppem =
# size * 150/72):
#
#   ui_8_regular   CozetteVector  size 10  (~21px)  -> status bar / hints (SMALL_FONT_ID)
#   ui_10_regular  CozetteVector  size 12  (~25px)  -> body / menus / buttons (UI_10_FONT_ID)
#   ui_12_regular  CozetteVector  size 14  (~29px)  -> titles / headers (UI_12_FONT_ID, distinct)
#
# Cozette ships regular only here, so there is no bold face; UI hierarchy comes
# from the three sizes, exactly as v11 did. Re-run through the CURRENT
# fontconvert.py so the header is the current EpdFontData format (the v11 headers
# were an older, incompatible struct layout that rendered as garbage).
#
# NOTE on weight: v11 used the historic --bw-threshold 2 ("any coverage = black"),
# but the CURRENT renderer already dilated 1-bit glyphs by +1px at the crisp
# default (a bug, since fixed in GfxRenderer.cpp). With that dilation gone we bake
# at --bw-threshold 8 (true regular weight) so the face reads thin/clean like v11
# rather than doubly-fattened.
set -e
cd "$(dirname "$0")"
PYTHON_BIN="${PYTHON_BIN:-python3}"

SRC="../builtinFonts/source/UI"
for f in CozetteVector.ttf; do
  if [[ ! -f "$SRC/$f" ]]; then
    echo "error: missing source font: $SRC/$f" >&2
    exit 1
  fi
done

gen() { # <out-header-name> <size> <ttf>   (dpi 150 default = v11 ppem; threshold 8 = regular weight)
  echo "Generating $1 from $(basename "$3") @ size ${2} (threshold 8)..."
  "$PYTHON_BIN" fontconvert.py "$1" "$2" "$SRC/$3" --bw-threshold 8 > "../builtinFonts/$1.h"
}

gen ui_8_regular  10 CozetteVector.ttf   # status bar
gen ui_10_regular 12 CozetteVector.ttf   # body / menus
gen ui_12_regular 14 CozetteVector.ttf   # titles

echo ""
echo "Generated 3 UI font headers (Cozette, v11.0.0 sizes 10/12/14, default flags)."
