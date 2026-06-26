#!/bin/bash
# Narrow-scope font conversion: regenerate ONLY the Pixel32 variants.
# Does NOT invoke convert-builtin-fonts.sh or regenerate any other font.
#
# Pixel32 (Pix32) is a pixel display reader font. Sizes 12, 14, 16, three styles
# (Regular, Bold, Italic; bold-italic synthesized via nullptr slot). Ships full
# prose punctuation incl. the pipe glyph, so no source patching needed.
set -e
cd "$(dirname "$0")"
PYTHON_BIN="${PYTHON_BIN:-python3}"
SIZES=(12 14 16)
SOURCE_DIR="../builtinFonts/source/Pixel32"
declare -a STYLES=( "regular:Pixel32-Regular.ttf" "bold:Pixel32-Bold.ttf" "italic:Pixel32-Italic.ttf" )
for SIZE in "${SIZES[@]}"; do
  for entry in "${STYLES[@]}"; do
    style="${entry%%:*}"; ttf="${entry#*:}"; source_path="${SOURCE_DIR}/${ttf}"
    [[ -f "$source_path" ]] || { echo "error: missing $source_path" >&2; exit 1; }
    font_name="pixel32_${SIZE}_${style}"
    echo "Generating ${font_name} from ${ttf}..."
    "$PYTHON_BIN" fontconvert.py "${font_name}" "$SIZE" "$source_path" --2bit --compress > "../builtinFonts/${font_name}.h"
  done
done
echo ""; echo "Generated $(( ${#SIZES[@]} * ${#STYLES[@]} )) Pixel32 headers."
"$PYTHON_BIN" "$(dirname "$0")/dedup-shared-tables.py" >/dev/null 2>&1 || true
