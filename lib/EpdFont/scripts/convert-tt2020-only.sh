#!/bin/bash
# Narrow-scope font conversion: regenerate ONLY the TT2020 variants.
#
# Does NOT invoke convert-builtin-fonts.sh or regenerate any other font.
# Running the full pipeline in the past caused UI bitmap drift (Cozette
# re-rendered, device UI visibly changed after flash).
#
# TT2020 is a typewriter-emulation font family. We expose ONE size (15)
# and FOUR faces using two source TTFs per style:
#   Regular    -> TT2020Base-Regular.ttf
#   Italic     -> TT2020Base-Italic.ttf
#   Bold       -> TT2020StyleE-Regular.ttf   (Style E is the "heavier"
#   BoldItalic -> TT2020StyleE-Italic.ttf    worn variant; stands in for bold.)
#
set -e
cd "$(dirname "$0")"
PYTHON_BIN="${PYTHON_BIN:-python3}"

SIZES=(15 17)
SOURCE_DIR="../builtinFonts/source/TT2020"

declare -a FACES=(
  "regular:TT2020Base-Regular.ttf"
  "italic:TT2020Base-Italic.ttf"
  "bold:TT2020StyleE-Regular.ttf"
  "bolditalic:TT2020StyleE-Italic.ttf"
)

for SIZE in "${SIZES[@]}"; do
  for entry in "${FACES[@]}"; do
    face="${entry%%:*}"
    ttf="${entry#*:}"
    source_path="${SOURCE_DIR}/${ttf}"
    if [[ ! -f "$source_path" ]]; then
      echo "error: missing source TTF: $source_path" >&2
      exit 1
    fi
    font_name="tt2020_${SIZE}_${face}"
    echo "Generating ${font_name} from ${ttf}..."
    "$PYTHON_BIN" fontconvert.py "${font_name}" "$SIZE" "$source_path" --2bit --compress \
      > "../builtinFonts/${font_name}.h"
  done
done

echo ""
echo "Generated $(( ${#SIZES[@]} * ${#FACES[@]} )) TT2020 headers across sizes ${SIZES[*]}."
echo ""
echo "Running dedup-shared-tables.py (TT2020 included if registered)..."
"$PYTHON_BIN" "$(dirname "$0")/dedup-shared-tables.py"
