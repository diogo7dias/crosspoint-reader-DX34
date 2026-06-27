#!/bin/bash
# Narrow-scope font conversion: regenerate ONLY the Lato variants.
#
# Does NOT invoke convert-builtin-fonts.sh or regenerate any other font.
#
# Lato is a humanist sans-serif reader font. Generated at sizes
# 10, 12, 14, 16, 17 (matching Georgia/ChareInk/Bookerly) with FOUR
# styles (Regular, Bold, Italic, BoldItalic) -- all four real faces baked. Lato
# ships full prose punctuation incl. the pipe glyph (fontconvert derives metrics
# from '|'), so no source patching needed.
#
# dedup-shared-tables.py is deliberately NOT called (Lato is not in its hardcoded
# family list; skipping keeps this script from touching other families' shared
# tables). Lato keeps self-contained per-size tables.
set -e
cd "$(dirname "$0")"
PYTHON_BIN="${PYTHON_BIN:-python3}"

SIZES=(10 12 14 16 17)
SOURCE_DIR="../builtinFonts/source/Lato"

declare -a STYLES=(
  "regular:Lato-Regular.ttf"
  "bold:Lato-Bold.ttf"
  "italic:Lato-Italic.ttf"
  "bolditalic:Lato-BoldItalic.ttf"
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
    font_name="lato_${SIZE}_${style}"
    echo "Generating ${font_name} from ${ttf}..."
    "$PYTHON_BIN" fontconvert.py "${font_name}" "$SIZE" "$source_path" --2bit --compress \
      > "../builtinFonts/${font_name}.h"
  done
done

echo ""
echo "Generated $(( ${#SIZES[@]} * ${#STYLES[@]} )) Lato headers."
