#!/bin/bash
# Narrow-scope font conversion: regenerate ONLY the Playfair Display variants.
#
# Does NOT invoke convert-builtin-fonts.sh or regenerate any other font.
#
# Playfair Display is a high-contrast transitional serif (Google Fonts, SIL OFL).
# It ships as an SD-only family (Tier-1: glyph tables in flash, bitmap blobs
# streamed from the /fonts/*.bin packs) — there is NO in-flash bitmap fallback in
# the `default` build, so every size is gated behind CROSSPOINT_SD_FONTS in
# main.cpp. Generated at sizes 10..18 with four REAL styles (Regular, Bold,
# Italic, BoldItalic) — the italic variable font carries its own wght axis so
# bold-italic is a real instance, not synthesised (Lato pattern).
#
# NOTE Playfair is a DISPLAY face: hairline-thin high-contrast strokes. At the
# smallest reader sizes (10..12) the thin stems may render fragile on a 1-bit grid;
# the Smooth Text (anti-aliasing) toggle helps. This is inherent to the typeface.
#
# SOURCE PREP (one-time; the committed static TTFs in source/PlayfairDisplay/ are
# the result — re-run only if you need to re-derive from the upstream variable
# fonts):
#   The upstream files are variable fonts with a single wght(400..900) axis. We pin
#   wght 400 (Regular) / 700 (Bold) on the upright and italic files:
#     fonttools varLib.instancer PlayfairDisplay[wght].ttf        wght=400 -o PlayfairDisplay-Regular.ttf
#     fonttools varLib.instancer PlayfairDisplay[wght].ttf        wght=700 -o PlayfairDisplay-Bold.ttf
#     fonttools varLib.instancer PlayfairDisplay-Italic[wght].ttf wght=400 -o PlayfairDisplay-Italic.ttf
#     fonttools varLib.instancer PlayfairDisplay-Italic[wght].ttf wght=700 -o PlayfairDisplay-BoldItalic.ttf
#   Then each static instance is subset to the reader charset (Latin / European /
#   punctuation / currency / math / arrows / ligatures — NO Cyrillic, NO Greek) AND
#   has its GPOS table dropped (see convert-merriweather-only.sh for the rationale;
#   same uint8_t kern-class overflow + zero perceptible loss at reader ppem):
#     UNI="0000-024F,0300-036F,1EA0-1EF9,2000-20CF,2190-22FF,FB00-FB06,FFFD"
#     fonttools subset <instance>.ttf --unicodes="$UNI" --drop-tables+=GPOS --output-file=<instance>.ttf
set -e
cd "$(dirname "$0")"
PYTHON_BIN="${PYTHON_BIN:-python3}"

SIZES=(10 11 12 13 14 15 16 17 18)
SOURCE_DIR="../builtinFonts/source/PlayfairDisplay"

declare -a STYLES=(
  "regular:PlayfairDisplay-Regular.ttf"
  "bold:PlayfairDisplay-Bold.ttf"
  "italic:PlayfairDisplay-Italic.ttf"
  "bolditalic:PlayfairDisplay-BoldItalic.ttf"
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
    font_name="playfair_${SIZE}_${style}"
    echo "Generating ${font_name} from ${ttf}..."
    "$PYTHON_BIN" fontconvert.py "${font_name}" "$SIZE" "$source_path" --2bit --compress \
      > "../builtinFonts/${font_name}.h"
    generated+=("../builtinFonts/${font_name}.h")
  done
done

echo ""
echo "Generated ${#generated[@]} Playfair Display headers."
echo ""
echo "Offloading bitmap arrays behind CROSSPOINT_SD_FONTS_SLIM (SD-streamed)..."
"$PYTHON_BIN" offload_bitmaps.py "${generated[@]}"
