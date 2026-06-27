#!/bin/bash
# Narrow-scope font conversion: regenerate ONLY the Verdana reader variants.
#
# Does NOT invoke convert-builtin-fonts.sh or regenerate any other font (no UI
# bitmap drift).
#
# Verdana is a humanist sans-serif reader font designed for on-screen legibility
# (wide, open letterforms). Generated at sizes 10, 12, 14, 16, 17 with
# three faces (Regular, Bold, Italic). No BoldItalic source, so that slot is
# nullptr and the renderer synthesises bold-italic (Georgia pattern). Ships full
# prose punctuation incl. the pipe glyph (fontconvert derives metrics from '|').
#
# dedup-shared-tables.py is deliberately NOT called (Verdana is not in its
# hardcoded family list). Verdana keeps self-contained per-size tables.
set -e
cd "$(dirname "$0")"
PYTHON_BIN="${PYTHON_BIN:-python3}"

SIZES=(10 12 14 16 17)
SOURCE_DIR="../builtinFonts/source/Verdana"

declare -a STYLES=(
  "regular:Verdana-Regular.ttf"
  "bold:Verdana-Bold.ttf"
  "italic:Verdana-Italic.ttf"
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
    font_name="verdana_${SIZE}_${style}"
    echo "Generating ${font_name} from ${ttf}..."
    "$PYTHON_BIN" fontconvert.py "${font_name}" "$SIZE" "$source_path" --2bit --compress \
      > "../builtinFonts/${font_name}.h"
  done
done

echo ""
echo "Generated $(( ${#SIZES[@]} * ${#STYLES[@]} )) Verdana headers."
