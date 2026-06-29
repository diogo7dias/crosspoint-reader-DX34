#!/bin/bash
# Narrow-scope font conversion: regenerate ONLY the Galmuri variants.
#
# Does NOT invoke convert-builtin-fonts.sh or regenerate any other font.
#
# Galmuri is a PIXEL font (Galmuri14 cut, designed on a 14px grid; pixel-style
# vector outlines, em=1500). Unlike the smooth outline reader fonts it is crisp
# ONLY when the pixel grid lands on whole screen pixels — i.e. at ppem that is an
# integer multiple of 14. So it is rendered with --dpi 72 (ppem == size) at the
# two native sizes 14 (1x) and 28 (2x); the reader maps the 10..18 size scale onto
# these (10..15 -> 14px, 16..18 -> 28px) since a pixel font cannot do in-between
# sizes crisply. SD-only family (tables in flash, bitmaps streamed from SD packs),
# gated behind CROSSPOINT_SD_FONTS in main.cpp.
#
# Three real weights (Regular/Bold/Italic); bold-italic synthesised by
# EpdFontFamily (Georgia pattern). --2bit --compress matches the SD-pack pipeline;
# at the native grid the 2-bit output is effectively binary (pixel-font stems land
# on pixel boundaries) and the reader thresholds to 1-bit on render, so it stays
# crisp. Source = the Latin-subset of the upstream Galmuri14 TTFs (CJK/kana
# dropped — the user reads EN/PT; subset also strips the would-be huge tables):
#   UNI="0000-024F,0300-036F,1EA0-1EF9,2000-20CF,2190-22FF,FB00-FB06,FFFD"
#   fonttools subset Galmuri14<Weight>.ttf --unicodes="$UNI" --drop-tables+=GPOS --output-file=Galmuri14-<Weight>.ttf
set -e
cd "$(dirname "$0")"
PYTHON_BIN="${PYTHON_BIN:-python3}"

SIZES=(14 28)
SOURCE_DIR="../builtinFonts/source/Galmuri"

declare -a STYLES=(
  "regular:Galmuri14-Regular.ttf"
  "bold:Galmuri14-Bold.ttf"
  "italic:Galmuri14-Italic.ttf"
)

generated=()
for SIZE in "${SIZES[@]}"; do
  for entry in "${STYLES[@]}"; do
    style="${entry%%:*}"
    ttf="${entry#*:}"
    source_path="${SOURCE_DIR}/${ttf}"
    if [[ ! -f "$source_path" ]]; then
      echo "error: missing source TTF: $source_path" >&2
      exit 1
    fi
    font_name="galmuri_${SIZE}_${style}"
    echo "Generating ${font_name} from ${ttf} (--dpi 72 native pixel grid)..."
    "$PYTHON_BIN" fontconvert.py "${font_name}" "$SIZE" "$source_path" --2bit --compress --dpi 72 \
      > "../builtinFonts/${font_name}.h"
    generated+=("../builtinFonts/${font_name}.h")
  done
done

echo ""
echo "Generated ${#generated[@]} Galmuri headers."
echo ""
echo "Offloading bitmap arrays behind CROSSPOINT_SD_FONTS_SLIM (SD-streamed)..."
"$PYTHON_BIN" offload_bitmaps.py "${generated[@]}"
