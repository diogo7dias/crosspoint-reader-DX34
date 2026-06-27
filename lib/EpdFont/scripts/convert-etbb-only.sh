#!/bin/bash
# Narrow-scope font conversion: regenerate ONLY the ETbb (ET Book) variants.
#
# Does NOT invoke convert-builtin-fonts.sh or regenerate any other font.
#
# ETbb (ET Book / ET Bembo) is a serif reader font. Generated at sizes
# 10, 12, 13, 14, 15, 16, 17 (matching Georgia/ChareInk/Bookerly) with FOUR
# styles (Regular, Bold, Italic, BoldItalic). Unlike Georgia, ETbb ships a real
# BoldItalic face, so the family ctor uses it directly instead of synthesising.
# ETbb ships full prose punctuation incl. the pipe glyph (fontconvert derives
# metrics from '|'), so no source patching needed.
#
# dedup-shared-tables.py is deliberately NOT called: ETbb is not in its hardcoded
# family list, so it would be skipped anyway, and skipping the call keeps this
# script from touching any other family's shared tables. ETbb therefore keeps
# self-contained per-size tables (same as Georgia/F25/Pixel32).
set -e
cd "$(dirname "$0")"
PYTHON_BIN="${PYTHON_BIN:-python3}"

SIZES=(10 12 13 14 15 16 17)
SOURCE_DIR="../builtinFonts/source/ETbb"

declare -a STYLES=(
  "regular:ETbb-Regular.otf"
  "bold:ETbb-Bold.otf"
  "italic:ETbb-Italic.otf"
  "bolditalic:ETbb-BoldItalic.otf"
)

for SIZE in "${SIZES[@]}"; do
  for entry in "${STYLES[@]}"; do
    style="${entry%%:*}"
    ttf="${entry#*:}"
    source_path="${SOURCE_DIR}/${ttf}"
    if [[ ! -f "$source_path" ]]; then
      echo "error: missing source font: $source_path" >&2
      exit 1
    fi
    font_name="etbb_${SIZE}_${style}"
    echo "Generating ${font_name} from ${ttf}..."
    "$PYTHON_BIN" fontconvert.py "${font_name}" "$SIZE" "$source_path" --2bit --compress \
      > "../builtinFonts/${font_name}.h"
  done
done

echo ""
echo "Generated $(( ${#SIZES[@]} * ${#STYLES[@]} )) ETbb headers."
