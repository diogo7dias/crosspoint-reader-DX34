#!/bin/bash
# Bake Tier-2 CPBN font packs for the EXTRA reader sizes (11, 13, 15, 18) of
# every built-in reader family. Unlike the Tier-1 packs (bitmap-only, tables in
# flash), these carry the glyph / interval / group / kerning / ligature TABLES in
# the .bin too (exportFontBlobWithTables), so each new size costs ~0 flash — the
# firmware never compiles a header for them. The .bins are published to the
# `fonts` orphan branch for the browser "Get Font Packs" download.
#
# Scoped + drift-safe: generates ONLY the new sizes into a scratch dir; it never
# touches the committed 10/12/14/16/17 headers and never runs dedup, so the
# existing flash fonts cannot drift. Each pack is round-tripped through the real
# Tier-2 loader before it is written, so a corrupt/unloadable pack is never
# published.
#
# Usage:  bake-sd-extra-packs.sh <output-dir>
#   e.g.  bash lib/EpdFont/scripts/bake-sd-extra-packs.sh /tmp/sdpacks
set -euo pipefail
cd "$(dirname "$0")"
SCRIPT_DIR="$(pwd)"
EPDFONT_DIR="$(cd .. && pwd)"
PYTHON_BIN="${PYTHON_BIN:-python3}"

OUT="${1:?usage: bake-sd-extra-packs.sh <output-dir>}"
mkdir -p "$OUT"
SCRATCH="$(mktemp -d)"
HDRS="$SCRATCH/headers"
mkdir -p "$HDRS"
trap 'rm -rf "$SCRATCH"' EXIT

SIZES=(11 13 15 18)
SRC_BASE="../builtinFonts/source"

# family | source-subdir | weight:ttf[,weight:ttf...]
# Weight sets mirror each family's committed faces (Lato has a real bold-italic;
# the others synthesise it, so no bolditalic pack). Source TTFs + flags match the
# scoped convert-*-only.sh scripts exactly, so the new sizes bake like the rest.
FAMILIES=(
  "bookerly|Bookerly|regular:Bookerly.ttf,bold:Bookerly Bold.ttf,italic:Bookerly Italic.ttf"
  "georgia|Georgia|regular:Georgia-Regular.ttf,bold:Georgia-Bold.ttf,italic:Georgia-Italic.ttf"
  "lato|Lato|regular:Lato-Regular.ttf,bold:Lato-Bold.ttf,italic:Lato-Italic.ttf,bolditalic:Lato-BoldItalic.ttf"
  "helvetica|Helvetica|regular:Helvetica-Regular.ttf,bold:Helvetica-Bold.ttf,italic:Helvetica-Italic.ttf"
  "verdana|Verdana|regular:Verdana-Regular.ttf,bold:Verdana-Bold.ttf,italic:Verdana-Italic.ttf"
)

INCLUDES=""
ROWS=""
COUNT=0
for fam_entry in "${FAMILIES[@]}"; do
  IFS='|' read -r fam subdir weights <<<"$fam_entry"
  IFS=',' read -ra wlist <<<"$weights"
  for size in "${SIZES[@]}"; do
    for w in "${wlist[@]}"; do
      weight="${w%%:*}"
      ttf="${w#*:}"
      name="${fam}_${size}_${weight}"
      src="${SRC_BASE}/${subdir}/${ttf}"
      [[ -f "$src" ]] || { echo "error: missing source TTF: $src" >&2; exit 1; }
      echo "fontconvert ${name}..."
      "$PYTHON_BIN" fontconvert.py "$name" "$size" "$src" --2bit --compress >"$HDRS/${name}.h"
      INCLUDES+="#include \"headers/${name}.h\"
"
      case "$weight" in
        regular) v=0 ;; bold) v=1 ;; italic) v=2 ;; bolditalic) v=3 ;;
      esac
      ROWS+="  {&${name}, ${size}, ${v}, \"${name}.bin\"},
"
      COUNT=$((COUNT + 1))
    done
  done
done

# Emit the host pack tool: export each font WITH tables, verify it loads back
# through the real Tier-2 loader (glyph count must match), then write the .bin.
TOOL="$SCRATCH/pack_tool.cpp"
{
  echo "#include <cstdint>"
  echo "#include <cstdio>"
  echo "#include <string>"
  echo "#include <vector>"
  printf '%s' "$INCLUDES"
  echo "#include \"EpdBinExport.h\""
  echo "#include \"EpdBinTablesFont.h\""
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
  echo "    if (!exportFontBlobWithTables(*r.fd, r.variant, r.size, sink)) { fprintf(stderr, \"export FAIL %s\\n\", r.name); ++fail; continue; }"
  echo "    MemSrc src(sink.b);"
  echo "    EpdBinTablesFont tf;"
  echo "    if (tf.open(&src) != kOk || glyphCountFromIntervals(*tf.font()) != glyphCountFromIntervals(*r.fd)) { fprintf(stderr, \"VERIFY FAIL %s\\n\", r.name); ++fail; continue; }"
  echo "    std::string path = dir + \"/\" + r.name;"
  echo "    FILE* f = fopen(path.c_str(), \"wb\");"
  echo "    if (!f || fwrite(sink.b.data(), 1, sink.b.size(), f) != sink.b.size()) { fprintf(stderr, \"write FAIL %s\\n\", r.name); if (f) fclose(f); ++fail; continue; }"
  echo "    fclose(f);"
  echo "    ++ok;"
  echo "  }"
  echo "  printf(\"baked + verified %d packs, %d failed\\n\", ok, fail);"
  echo "  return fail ? 1 : 0;"
  echo "}"
} >"$TOOL"

echo "Compiling pack tool ($COUNT fonts)..."
g++ -std=c++17 -I "$EPDFONT_DIR" -I "$SCRATCH" "$TOOL" -o "$SCRATCH/pack_tool"
"$SCRATCH/pack_tool" "$OUT"
echo "Packs written to: $OUT"
ls "$OUT"/*.bin | wc -l | xargs echo "Total .bin files in output dir:"
