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

for size in 14 18; do
  echo "Generating unifont_${size}_regular..."
  "$PYTHON_BIN" fontconvert.py "unifont_${size}_regular" $size "../builtinFonts/source/UI/unifont-english.ttf" > "../builtinFonts/unifont_${size}_regular.h"
done
# Cozette UI fonts: the file name indicates the nominal font size, but the actual
# FreeType point size passed to fontconvert is +2. This mapping is intentional and
# was established when the UI first shipped — the device UI is tuned to these
# exact pixel metrics. Do not "fix" the mismatch by passing $size directly; doing
# so shrinks every UI element and regresses status bar, menus, and settings.
# (A 2026-06 round of UI-font experiments — Cairopixel/Pixel32/Ubuntu/Silkscreen/
# VT323/Inter — was reverted; Cozette covers the UI glyphs directly, no graft.)
UI_FONT_NAMES=(8 10 12)
UI_FONT_RENDER_SIZES=(10 12 14)
for i in "${!UI_FONT_NAMES[@]}"; do
  name_size="${UI_FONT_NAMES[$i]}"
  render_size="${UI_FONT_RENDER_SIZES[$i]}"
  echo "Generating ui_${name_size}_regular (render at ${render_size}pt)..."
  "$PYTHON_BIN" fontconvert.py "ui_${name_size}_regular" $render_size "../builtinFonts/source/UI/CozetteVector.ttf" > "../builtinFonts/ui_${name_size}_regular.h"
done

echo "Running dedup-shared-tables.py..."
"$PYTHON_BIN" "$(dirname "$0")/dedup-shared-tables.py"

echo "All fonts generated."
