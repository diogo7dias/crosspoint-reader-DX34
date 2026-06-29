#!/bin/bash
# Narrow-scope font conversion: regenerate ONLY the Vollkorn variants.
#
# Does NOT invoke convert-builtin-fonts.sh or regenerate any other font.
#
# Vollkorn is a serif reader font (Friedrich Althausen, SIL OFL). It ships as an
# SD-only family (Tier-1: glyph tables in flash, bitmap blobs streamed from the
# /fonts/*.bin packs), gated behind CROSSPOINT_SD_FONTS in main.cpp. Generated at
# sizes 10..18 with four REAL styles (Regular, Bold, Italic, BoldItalic).
#
# SOURCE PREP (one-time; the committed static TTFs in source/Vollkorn/ are the
# result): subset each upstream weight to the reader charset (Latin / European /
# punctuation / currency / math / arrows / ligatures — NO Cyrillic, NO Greek).
# Unlike Merriweather/Playfair, Vollkorn's class-based kerning is small (~120 kern
# classes, well under the format's uint8_t cap), so GPOS is KEPT — real pair
# kerning, matching the other built-in serifs (Georgia/Bookerly):
#   UNI="0000-024F,0300-036F,1EA0-1EF9,2000-20CF,2190-22FF,FB00-FB06,FFFD"
#   fonttools subset vollkorn.<weight>.ttf --unicodes="$UNI" --output-file=Vollkorn-<Weight>.ttf
set -e
cd "$(dirname "$0")"
PYTHON_BIN="${PYTHON_BIN:-python3}"

SIZES=(10 11 12 13 14 15 16 17 18)
SOURCE_DIR="../builtinFonts/source/Vollkorn"

declare -a STYLES=(
  "regular:Vollkorn-Regular.ttf"
  "bold:Vollkorn-Bold.ttf"
  "italic:Vollkorn-Italic.ttf"
  "bolditalic:Vollkorn-BoldItalic.ttf"
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
    font_name="vollkorn_${SIZE}_${style}"
    echo "Generating ${font_name} from ${ttf}..."
    "$PYTHON_BIN" fontconvert.py "${font_name}" "$SIZE" "$source_path" --2bit --compress \
      > "../builtinFonts/${font_name}.h"
    generated+=("../builtinFonts/${font_name}.h")
  done
done

echo ""
echo "Generated ${#generated[@]} Vollkorn headers."
echo ""
echo "Offloading bitmap arrays behind CROSSPOINT_SD_FONTS_SLIM (SD-streamed)..."
"$PYTHON_BIN" offload_bitmaps.py "${generated[@]}"
