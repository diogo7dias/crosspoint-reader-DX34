#!/bin/bash
# Bake Tier-1 CPBN font packs (bitmap-only) for the SD-only PIXEL family Galmuri,
# its two native crisp sizes 14 (1x) + 28 (2x), three weights (Regular/Bold/
# Italic). Same Tier-1 model as the other SD-only families: glyph TABLES compiled
# into flash (committed headers, offload_bitmaps.py drops the bitmap under slim),
# bitmaps streamed from these .bin packs on the SD card (~0 added heap).
#
# Source of truth = the committed headers (galmuri_{14,28}_*.h). No re-baking from
# TTF here; each pack is round-tripped through the real Tier-1 loader
# (EpdBinFontLoader) before it is written. These publish to the `fonts` GitHub
# branch for the browser-assisted "Get Font Packs" download flow.
#
# Usage:  bake-sd-galmuri-packs.sh <output-dir>
set -euo pipefail
cd "$(dirname "$0")"
EPDFONT_DIR="$(cd .. && pwd)"

OUT="${1:?usage: bake-sd-galmuri-packs.sh <output-dir>}"
mkdir -p "$OUT"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

SIZES=(14 28)

# family | weight:variant[,weight:variant...]   (variant byte: regular=0 bold=1 italic=2 bolditalic=3)
FAMILIES=(
  "galmuri|regular:0,bold:1,italic:2"
)

INCLUDES=""
ROWS=""
COUNT=0
for fam_entry in "${FAMILIES[@]}"; do
  IFS='|' read -r fam weights <<<"$fam_entry"
  IFS=',' read -ra wlist <<<"$weights"
  for size in "${SIZES[@]}"; do
    for w in "${wlist[@]}"; do
      weight="${w%%:*}"
      variant="${w#*:}"
      name="${fam}_${size}_${weight}"
      hdr="../builtinFonts/${name}.h"
      [[ -f "$hdr" ]] || { echo "error: missing committed header: $hdr" >&2; exit 1; }
      INCLUDES+="#include <builtinFonts/${name}.h>
"
      ROWS+="  {&${name}, ${size}, ${variant}, \"${name}.bin\"},
"
      COUNT=$((COUNT + 1))
    done
  done
done

TOOL="$SCRATCH/pack_tool.cpp"
{
  echo "#include <cstdint>"
  echo "#include <cstdio>"
  echo "#include <string>"
  echo "#include <vector>"
  printf '%s' "$INCLUDES"
  echo "#include \"EpdBinExport.h\""
  echo "#include \"EpdBinFontLoader.h\""
  echo "using namespace crosspoint::binfont;"
  echo "struct VecSink : ByteSink { std::vector<uint8_t> b; bool write(const uint8_t* d, size_t n) override { b.insert(b.end(), d, d + n); return true; } };"
  echo "struct MemSrc : BlobSource { std::vector<uint8_t> b; explicit MemSrc(std::vector<uint8_t> v) : b(std::move(v)) {} int read(uint32_t o, uint8_t* d, size_t n) override { if ((uint64_t)o + n > b.size()) return -1; for (size_t i = 0; i < n; ++i) d[i] = b[o + i]; return (int)n; } uint32_t size() const override { return (uint32_t)b.size(); } };"
  echo "struct Row { const EpdFontData* fd; uint16_t size; uint8_t variant; const char* name; };"
  echo "static const Row kRows[] = {"
  printf '%s' "$ROWS"
  echo "};"
  echo "int main(int argc, char** argv) {"
  echo "  if (argc < 2) { fprintf(stderr, \"usage: pack_tool <outdir>\\n\"); return 2; }"
  echo "  std::string dir = argv[1];"
  echo "  int ok = 0, fail = 0;"
  echo "  for (const auto& r : kRows) {"
  echo "    VecSink sink;"
  echo "    if (!exportFontBlob(*r.fd, r.variant, r.size, sink)) { fprintf(stderr, \"export FAIL %s\\n\", r.name); ++fail; continue; }"
  echo "    MemSrc src(sink.b);"
  echo "    EpdBinFontLoader loader;"
  echo "    const FontBlobExpectation expect{glyphCountFromIntervals(*r.fd), r.fd->intervalCount, r.fd->groupCount};"
  echo "    if (loader.open(&src, expect) != kOk) { fprintf(stderr, \"VERIFY FAIL %s\\n\", r.name); ++fail; continue; }"
  echo "    std::string path = dir + \"/\" + r.name;"
  echo "    FILE* f = fopen(path.c_str(), \"wb\");"
  echo "    if (!f || fwrite(sink.b.data(), 1, sink.b.size(), f) != sink.b.size()) { fprintf(stderr, \"write FAIL %s\\n\", r.name); if (f) fclose(f); ++fail; continue; }"
  echo "    fclose(f);"
  echo "    ++ok;"
  echo "  }"
  echo "  printf(\"baked + verified %d Tier-1 packs, %d failed\\n\", ok, fail);"
  echo "  return fail ? 1 : 0;"
  echo "}"
} >"$TOOL"

echo "Compiling pack tool ($COUNT fonts; bitmaps present = no _SLIM)..."
g++ -std=c++17 -I "$EPDFONT_DIR" "$TOOL" -o "$SCRATCH/pack_tool"
"$SCRATCH/pack_tool" "$OUT"
echo "Packs written to: $OUT"
ls "$OUT"/*.bin | wc -l | xargs echo "Total .bin files in output dir:"
