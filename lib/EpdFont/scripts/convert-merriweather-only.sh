#!/bin/bash
# Narrow-scope font conversion: regenerate ONLY the Merriweather variants.
#
# Does NOT invoke convert-builtin-fonts.sh or regenerate any other font.
#
# Merriweather is a serif reader font (Google Fonts, SIL OFL). It ships as an
# SD-only family (Tier-1: glyph tables compiled into flash, bitmap blobs streamed
# from the /fonts/*.bin packs) — there is NO in-flash bitmap fallback in the
# `default` build, so every size is gated behind CROSSPOINT_SD_FONTS in main.cpp.
# Generated at sizes 10..18 with four REAL styles (Regular, Bold, Italic,
# BoldItalic) — the italic variable font carries its own wght axis so bold-italic
# is a real instance, not synthesised (Lato pattern).
#
# SOURCE PREP (one-time; the committed static TTFs in source/Merriweather/ are the
# result — re-run only if you need to re-derive from the upstream variable fonts):
#   The upstream files are variable fonts with axes wght(300..900) wdth(87..112)
#   opsz(18..144). We pin opsz=18 (the body-text optical size — best legibility at
#   reader ppem), wdth=100 (normal), and wght 400 (Regular) / 700 (Bold):
#     fonttools varLib.instancer Merriweather[opsz,wdth,wght].ttf        wght=400 wdth=100 opsz=18 -o Merriweather-Regular.ttf
#     fonttools varLib.instancer Merriweather[opsz,wdth,wght].ttf        wght=700 wdth=100 opsz=18 -o Merriweather-Bold.ttf
#     fonttools varLib.instancer Merriweather-Italic[opsz,wdth,wght].ttf wght=400 wdth=100 opsz=18 -o Merriweather-Italic.ttf
#     fonttools varLib.instancer Merriweather-Italic[opsz,wdth,wght].ttf wght=700 wdth=100 opsz=18 -o Merriweather-BoldItalic.ttf
#   Then each static instance is subset to the reader charset (Latin / European /
#   punctuation / currency / math / arrows / ligatures — NO Cyrillic, NO Greek)
#   AND has its GPOS table dropped. Dropping GPOS removes pair-kerning: Merriweather
#   carries ~70k class-based kern pairs (728 kern classes) which overflow this
#   format's uint8_t class index and would bloat flash by a half-megabyte. The
#   existing built-in fonts barely kern (Georgia = 3 classes) and at 10..18px on a
#   1-/2-bit e-ink grid pair-kerning is imperceptible, so this is a no-loss prune:
#     UNI="0000-024F,0300-036F,1EA0-1EF9,2000-20CF,2190-22FF,FB00-FB06,FFFD"
#     fonttools subset <instance>.ttf --unicodes="$UNI" --drop-tables+=GPOS --output-file=<instance>.ttf
set -e
cd "$(dirname "$0")"
PYTHON_BIN="${PYTHON_BIN:-python3}"

SIZES=(10 11 12 13 14 15 16 17 18)
SOURCE_DIR="../builtinFonts/source/Merriweather"

declare -a STYLES=(
  "regular:Merriweather-Regular.ttf"
  "bold:Merriweather-Bold.ttf"
  "italic:Merriweather-Italic.ttf"
  "bolditalic:Merriweather-BoldItalic.ttf"
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
    font_name="merriweather_${SIZE}_${style}"
    echo "Generating ${font_name} from ${ttf}..."
    "$PYTHON_BIN" fontconvert.py "${font_name}" "$SIZE" "$source_path" --2bit --compress \
      > "../builtinFonts/${font_name}.h"
    generated+=("../builtinFonts/${font_name}.h")
  done
done

echo ""
echo "Generated ${#generated[@]} Merriweather headers."
echo ""
echo "Offloading bitmap arrays behind CROSSPOINT_SD_FONTS_SLIM (SD-streamed)..."
"$PYTHON_BIN" offload_bitmaps.py "${generated[@]}"
