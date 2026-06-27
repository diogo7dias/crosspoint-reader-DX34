#!/bin/bash
set -e
cd "$(dirname "$0")"
PYTHON_BIN="${PYTHON_BIN:-python3}"
READER_FONT_STYLES=("Regular" "Italic" "Bold")
CHAREINK_FONT_SIZES=(10 12 14 16 17)
for size in ${CHAREINK_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    lower_style=$(echo $style | tr '[:upper:]' '[:lower:]')
    font_name="chareink_${size}_${lower_style}"
    echo "Generating ${font_name}..."
    "$PYTHON_BIN" fontconvert.py "${font_name}" $size "../builtinFonts/source/ChareInk/ChareInk7SP-${style}.ttf" --2bit --compress > "../builtinFonts/${font_name}.h"
  done
done

BOOKERLY_FONT_SIZES=(10 12 14 16 17)
BOOKERLY_STYLES=("Regular:Bookerly.ttf" "Bold:Bookerly Bold.ttf" "Italic:Bookerly Italic.ttf")
for size in ${BOOKERLY_FONT_SIZES[@]}; do
  for entry in "${BOOKERLY_STYLES[@]}"; do
    style="${entry%%:*}"
    filename="${entry#*:}"
    lower_style=$(echo $style | tr '[:upper:]' '[:lower:]')
    font_name="bookerly_${size}_${lower_style}"
    echo "Generating ${font_name}..."
    "$PYTHON_BIN" fontconvert.py "${font_name}" $size "../builtinFonts/source/Bookerly/${filename}" --2bit --compress > "../builtinFonts/${font_name}.h"
  done
done

# Georgia: only regular, bold, italic (no bold-italic source)
VOLLKORN_FONT_SIZES=(12 13 14 15 16 17)
VOLLKORN_STYLES=("Regular:Vollkorn-Regular.ttf" "Bold:Vollkorn-Bold.ttf" "Italic:Vollkorn-Italic.ttf")
for size in ${VOLLKORN_FONT_SIZES[@]}; do
  for entry in "${VOLLKORN_STYLES[@]}"; do
    style="${entry%%:*}"
    filename="${entry#*:}"
    lower_style=$(echo $style | tr '[:upper:]' '[:lower:]')
    font_name="vollkorn_${size}_${lower_style}"
    echo "Generating ${font_name}..."
    "$PYTHON_BIN" fontconvert.py "${font_name}" $size "../builtinFonts/source/Vollkorn/${filename}" --2bit --compress > "../builtinFonts/${font_name}.h"
  done
done

for size in 14 18; do
  echo "Generating unifont_${size}_regular..."
  "$PYTHON_BIN" fontconvert.py "unifont_${size}_regular" $size "../builtinFonts/source/UI/unifont-english.ttf" > "../builtinFonts/unifont_${size}_regular.h"
done
# UI font = Pixel Operator (CC0 bitmap face). Two sizes: PixelOperator @16px for
# the status bar (16 == native grid -> pixel-perfect, threshold moot) and
# PixelOperator @32px regular/bold for body, menus and titles. Rendered with
# --dpi 72 so ppem == size. BOTH sizes are exact integer multiples of the 16px
# design grid (16 = 1x, 32 = 2x), so there is NO anti-aliasing and every stem is a
# uniform 1px / 2px block -> pixel-perfect and even (no fractional-pixel stem-width
# wobble). --bw-threshold is therefore moot here. See convert-ui-only.sh for the
# authoritative scoped rebake + rationale.
echo "Generating ui_16_regular from PixelOperator @16px..."
"$PYTHON_BIN" fontconvert.py "ui_16_regular" 16 "../builtinFonts/source/PixelOperator/PixelOperator.ttf" --dpi 72 --bw-threshold 8 > "../builtinFonts/ui_16_regular.h"
echo "Generating ui_32_regular from PixelOperator @32px..."
"$PYTHON_BIN" fontconvert.py "ui_32_regular" 32 "../builtinFonts/source/PixelOperator/PixelOperator.ttf" --dpi 72 --bw-threshold 8 > "../builtinFonts/ui_32_regular.h"
echo "Generating ui_32_bold from PixelOperator-Bold @32px..."
"$PYTHON_BIN" fontconvert.py "ui_32_bold" 32 "../builtinFonts/source/PixelOperator/PixelOperator-Bold.ttf" --dpi 72 --bw-threshold 8 > "../builtinFonts/ui_32_bold.h"

echo "Running dedup-shared-tables.py..."
"$PYTHON_BIN" "$(dirname "$0")/dedup-shared-tables.py"

echo "All fonts generated."
