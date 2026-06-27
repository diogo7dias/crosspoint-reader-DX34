#!/bin/bash
# Narrow-scope font conversion: regenerate ONLY the Rosarivo variants.
#
# Does NOT invoke convert-builtin-fonts.sh or regenerate any other font.
#
# Rosarivo is a Renaissance-style serif reader font. Generated at sizes
# 10, 12, 13, 14, 15, 16, 17 (matching Georgia/ChareInk/Bookerly) with only TWO
# styles (Regular, Italic) — Rosarivo ships no Bold or BoldItalic face. The
# family ctor uses nullptr for both bold slots, so a bold request falls back to
# the regular face (no synthetic bold). Rosarivo ships full prose punctuation
# incl. the pipe glyph (fontconvert derives metrics from '|'), so no source
# patching needed.
#
# dedup-shared-tables.py is deliberately NOT called (Rosarivo is not in its
# hardcoded family list; skipping keeps this script from touching other
# families' shared tables). Rosarivo keeps self-contained per-size tables.
set -e
cd "$(dirname "$0")"
PYTHON_BIN="${PYTHON_BIN:-python3}"

SIZES=(10 12 13 14 15 16 17)
SOURCE_DIR="../builtinFonts/source/Rosarivo"

declare -a STYLES=(
  "regular:Rosarivo-Regular.ttf"
  "italic:Rosarivo-Italic.ttf"
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
    font_name="rosarivo_${SIZE}_${style}"
    echo "Generating ${font_name} from ${ttf}..."
    "$PYTHON_BIN" fontconvert.py "${font_name}" "$SIZE" "$source_path" --2bit --compress \
      > "../builtinFonts/${font_name}.h"
  done
done

echo ""
echo "Generated $(( ${#SIZES[@]} * ${#STYLES[@]} )) Rosarivo headers."
