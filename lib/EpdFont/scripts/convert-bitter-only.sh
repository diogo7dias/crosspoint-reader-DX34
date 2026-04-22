#!/bin/bash
# Narrow-scope font conversion: regenerate ONLY the Bitter variants.
#
# Does NOT invoke convert-builtin-fonts.sh or regenerate any other font.
# Running the full pipeline in the past caused UI bitmap drift (Cozette
# re-rendered, device UI visibly changed after flash).
#
# Bitter is a slab-serif reader font. Generated at sizes 12-17 with three
# styles (Regular, Bold, Italic). No BoldItalic source TTF; family ctor
# uses nullptr for that slot, matching the Vollkorn pattern.
#
set -e
cd "$(dirname "$0")"
PYTHON_BIN="${PYTHON_BIN:-python3}"

SIZES=(12 14 16)
SOURCE_DIR="../builtinFonts/source/Bitter"

declare -a STYLES=(
  "regular:Bitter-Regular.ttf"
  "bold:Bitter-Bold.ttf"
  "italic:Bitter-Italic.ttf"
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
    font_name="bitter_${SIZE}_${style}"
    echo "Generating ${font_name} from ${ttf}..."
    "$PYTHON_BIN" fontconvert.py "${font_name}" "$SIZE" "$source_path" --2bit --compress \
      > "../builtinFonts/${font_name}.h"
  done
done

echo ""
echo "Generated $(( ${#SIZES[@]} * ${#STYLES[@]} )) Bitter headers."
echo ""
echo "Running dedup-shared-tables.py (Bitter included)..."
"$PYTHON_BIN" "$(dirname "$0")/dedup-shared-tables.py"
