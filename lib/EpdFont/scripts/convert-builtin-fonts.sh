#!/bin/bash
set -e
cd "$(dirname "$0")"
PYTHON_BIN="${PYTHON_BIN:-python3}"
READER_FONT_STYLES=("Regular" "Italic" "Bold")
CHAREINK_FONT_SIZES=(12 13 14 15 16 17)
for size in ${CHAREINK_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    lower_style=$(echo $style | tr '[:upper:]' '[:lower:]')
    font_name="chareink_${size}_${lower_style}"
    echo "Generating ${font_name}..."
    "$PYTHON_BIN" fontconvert.py "${font_name}" $size "../builtinFonts/source/ChareInk/ChareInk7SP-${style}.ttf" --2bit --compress > "../builtinFonts/${font_name}.h"
  done
done

BOOKERLY_FONT_SIZES=(12 13 14 15 16 17)
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

# IM Fell DW Pica: single reader size 15, Regular + Italic only.
# Bold is synthesized at runtime via EpdFontFamily syntheticBoldExtraPasses,
# so no bold source file or header is generated.
IMFELL_FONT_SIZES=(15)
IMFELL_STYLES=("Regular:IMFellDWPica-Regular.ttf" "Italic:IMFellDWPica-Italic.ttf")
for size in ${IMFELL_FONT_SIZES[@]}; do
  for entry in "${IMFELL_STYLES[@]}"; do
    style="${entry%%:*}"
    filename="${entry#*:}"
    lower_style=$(echo $style | tr '[:upper:]' '[:lower:]')
    font_name="imfell_${size}_${lower_style}"
    echo "Generating ${font_name}..."
    "$PYTHON_BIN" fontconvert.py "${font_name}" $size "../builtinFonts/source/IMFell/${filename}" --2bit --compress > "../builtinFonts/${font_name}.h"
  done
done

for size in 14 18; do
  echo "Generating unifont_${size}_regular..."
  "$PYTHON_BIN" fontconvert.py "unifont_${size}_regular" $size "../builtinFonts/source/UI/unifont-english.ttf" > "../builtinFonts/unifont_${size}_regular.h"
done
for size in 8 10 12; do
  echo "Generating ui_${size}_regular..."
  "$PYTHON_BIN" fontconvert.py "ui_${size}_regular" $size "../builtinFonts/source/UI/CozetteVector.ttf" > "../builtinFonts/ui_${size}_regular.h"
done

echo "All fonts generated."
