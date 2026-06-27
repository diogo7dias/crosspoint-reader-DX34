#!/bin/bash
# Narrow-scope font conversion: regenerate ONLY the Helvetica reader variants.
#
# Does NOT invoke convert-builtin-fonts.sh or regenerate any other font (no UI
# bitmap drift).
#
# Helvetica is a grotesque sans-serif reader font. Generated at sizes
# 10, 12, 14, 16, 17 with three faces (Regular, Bold, Italic). The
# italic is Helvetica's Oblique face (extracted from the macOS Helvetica.ttc).
# No BoldItalic source, so that slot is nullptr and the renderer synthesises
# bold-italic (Georgia pattern). Ships full prose punctuation incl. the pipe
# glyph (fontconvert derives metrics from '|').
#
# dedup-shared-tables.py is deliberately NOT called (Helvetica is not in its
# hardcoded family list). Helvetica keeps self-contained per-size tables.
set -e
cd "$(dirname "$0")"
PYTHON_BIN="${PYTHON_BIN:-python3}"

SIZES=(10 12 14 16 17)
SOURCE_DIR="../builtinFonts/source/Helvetica"

declare -a STYLES=(
  "regular:Helvetica-Regular.ttf"
  "bold:Helvetica-Bold.ttf"
  "italic:Helvetica-Italic.ttf"
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
    font_name="helvetica_${SIZE}_${style}"
    echo "Generating ${font_name} from ${ttf}..."
    "$PYTHON_BIN" fontconvert.py "${font_name}" "$SIZE" "$source_path" --2bit --compress \
      > "../builtinFonts/${font_name}.h"
  done
done

echo ""
echo "Generated $(( ${#SIZES[@]} * ${#STYLES[@]} )) Helvetica headers."
