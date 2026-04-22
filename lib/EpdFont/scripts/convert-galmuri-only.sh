#!/bin/bash
# Narrow-scope font conversion: regenerate ONLY the Galmuri variants.
#
# This script intentionally does NOT invoke convert-builtin-fonts.sh or
# regenerate any other font (Bookerly, Vollkorn, ChareInk, Unifont, UI
# fonts). Running the full pipeline in the past caused UI bitmap drift
# (Cozette re-rendered, device UI visibly changed after flash). Keep the
# scope tight to the one font being added.
#
# Galmuri is a Korean pixel bitmap font designed at 14 px native. We
# render it at sizes 11, 12, 14; sizes 11 and 12 are FreeType-downscaled
# from the 14 px grid (slight blur/jag) while 14 is pixel-native.
#
# Italic and Bold are synthesized by EpdFontFamily at draw time
# (slant + multi-pass redraw), so only `_regular` headers are generated.
set -e
cd "$(dirname "$0")"
PYTHON_BIN="${PYTHON_BIN:-python3}"

GALMURI_FONT_SIZES=(11 12 14)
SOURCE_TTF="../builtinFonts/source/Galmuri/Galmuri-Regular.ttf"

if [[ ! -f "$SOURCE_TTF" ]]; then
  echo "error: Galmuri source TTF not found at $SOURCE_TTF" >&2
  exit 1
fi

for size in "${GALMURI_FONT_SIZES[@]}"; do
  font_name="galmuri_${size}_regular"
  echo "Generating ${font_name}..."
  "$PYTHON_BIN" fontconvert.py "${font_name}" "$size" "$SOURCE_TTF" --2bit --compress \
    > "../builtinFonts/${font_name}.h"
done

echo ""
echo "Generated ${#GALMURI_FONT_SIZES[@]} Galmuri headers (Regular only;"
echo "Bold + Italic are synthesized at draw time)."
echo ""
echo "Running dedup-shared-tables.py (Galmuri included)..."
"$PYTHON_BIN" "$(dirname "$0")/dedup-shared-tables.py"
