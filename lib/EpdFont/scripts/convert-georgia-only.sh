#!/bin/bash
# Narrow-scope font conversion: regenerate ONLY the Georgia variants.
#
# Does NOT invoke convert-builtin-fonts.sh or regenerate any other font.
#
# Georgia is a serif reader font. Generated at sizes 10, 12, 13, 14, 15, 16, 17
# (matching ChareInk/Bookerly) with three styles (Regular, Bold, Italic). No
# BoldItalic source TTF; the family ctor uses nullptr for that slot and the
# renderer synthesises bold-italic (Vollkorn/Bitter pattern). Georgia ships full
# prose punctuation, so no source patching needed.
set -e
cd "$(dirname "$0")"
PYTHON_BIN="${PYTHON_BIN:-python3}"

SIZES=(10 12 13 14 15 16 17)
SOURCE_DIR="../builtinFonts/source/Georgia"

declare -a STYLES=(
  "regular:Georgia-Regular.ttf"
  "bold:Georgia-Bold.ttf"
  "italic:Georgia-Italic.ttf"
)

for SIZE in "${SIZES[@]}"; do
  for entry in "${STYLES[@]}"; do
    style="${entry%%:*}"
    ttf="${entry#*:}"
    source_path="${SOURCE_DIR}/${ttf}"
    if [[ ! -f "$source_path" ]]; then
      echo "error: missing source TTF: $source_path" >&2
      exit 1
    fi
    font_name="georgia_${SIZE}_${style}"
    echo "Generating ${font_name} from ${ttf}..."
    "$PYTHON_BIN" fontconvert.py "${font_name}" "$SIZE" "$source_path" --2bit --compress \
      > "../builtinFonts/${font_name}.h"
  done
done

echo ""
echo "Generated $(( ${#SIZES[@]} * ${#STYLES[@]} )) Georgia headers."
echo ""
echo "Running dedup-shared-tables.py (Georgia included)..."
"$PYTHON_BIN" "$(dirname "$0")/dedup-shared-tables.py"
