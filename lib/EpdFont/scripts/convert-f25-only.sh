#!/bin/bash
# Narrow-scope font conversion: regenerate ONLY the F25 Bank Printer variants.
#
# Does NOT invoke convert-builtin-fonts.sh or regenerate any other font.
# Running the full pipeline in the past caused UI bitmap drift (Cozette
# re-rendered, device UI visibly changed after flash).
#
# F25 Bank Printer is a stylised display reader font. Generated at sizes
# 10,11,12,13,14,16,17 with three styles (Regular, Bold, Italic). The BoldItalic
# source exists but is not baked; the family ctor uses nullptr for that slot and
# the renderer synthesises bold-italic (Vollkorn/Bitter pattern).
#
# The source TTFs in source/F25BankPrinter/ are PATCHED copies of the originals:
# em/en dash, ellipsis, bullet, nbsp and middot were synthesised in (the stock
# font lacks them) so prose renders without dropped punctuation. See
# scripts/patch_f25.py (kept in the session scratchpad) for the synthesis.
set -e
cd "$(dirname "$0")"
PYTHON_BIN="${PYTHON_BIN:-python3}"

SIZES=(10 11 12 13 14 16 17)
SOURCE_DIR="../builtinFonts/source/F25BankPrinter"

declare -a STYLES=(
  "regular:F25_Bank_Printer.ttf"
  "bold:F25_Bank_Printer-Bold.ttf"
  "italic:F25_Bank_Printer-Italic.ttf"
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
    font_name="f25_${SIZE}_${style}"
    echo "Generating ${font_name} from ${ttf}..."
    "$PYTHON_BIN" fontconvert.py "${font_name}" "$SIZE" "$source_path" --2bit --compress \
      > "../builtinFonts/${font_name}.h"
  done
done

echo ""
echo "Generated $(( ${#SIZES[@]} * ${#STYLES[@]} )) F25 headers."
echo ""
echo "Running dedup-shared-tables.py (F25 included)..."
"$PYTHON_BIN" "$(dirname "$0")/dedup-shared-tables.py"
