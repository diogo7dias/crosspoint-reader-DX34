// Host tool: regenerate the SD CPBN font packs from the in-flash builtin-font
// bitmaps, byte-identical to what the device's fat `sdfonts` build exports on
// boot. Used to publish the packs to the `fonts` GitHub branch (the browser
// font-download flow pulls them from there onto a fresh SD card).
//
// Build + run:
//   g++ -std=c++17 -I lib/EpdFont tools/pack_export.cpp -o /tmp/pack_export
//   /tmp/pack_export <out-dir>
//
// Source of truth = the same headers the firmware compiles; no re-baking from
// TTF, so the packs never drift from the flash glyph tables.
#include <cstdint>
#include <cstdio>
#include <string>

#include <builtinFonts/all.h>

#include "EpdBinExport.h"

using namespace crosspoint::binfont;

namespace {

struct FileSink : ByteSink {
  FILE* f = nullptr;
  bool write(const uint8_t* d, size_t n) override { return fwrite(d, 1, n, f) == n; }
};

struct Row {
  const EpdFontData* fd;
  uint16_t sizePt;
  const char* name;
};

// name = "<struct>.bin" so the file matches the device's /fonts/ path exactly.
#define P(sym, size) {&sym, size, #sym ".bin"}

const Row kRows[] = {
    P(bookerly_10_regular, 10),  P(bookerly_10_bold, 10),  P(bookerly_10_italic, 10),
    P(bookerly_12_regular, 12),  P(bookerly_12_bold, 12),  P(bookerly_12_italic, 12),
    P(bookerly_14_regular, 14),  P(bookerly_14_bold, 14),  P(bookerly_14_italic, 14),
    P(bookerly_16_regular, 16),  P(bookerly_16_bold, 16),  P(bookerly_16_italic, 16),
    P(bookerly_17_regular, 17),  P(bookerly_17_bold, 17),  P(bookerly_17_italic, 17),

    P(georgia_10_regular, 10),   P(georgia_10_bold, 10),   P(georgia_10_italic, 10),
    P(georgia_12_regular, 12),   P(georgia_12_bold, 12),   P(georgia_12_italic, 12),
    P(georgia_14_regular, 14),   P(georgia_14_bold, 14),   P(georgia_14_italic, 14),
    P(georgia_16_regular, 16),   P(georgia_16_bold, 16),   P(georgia_16_italic, 16),
    P(georgia_17_regular, 17),   P(georgia_17_bold, 17),   P(georgia_17_italic, 17),

    P(lato_10_regular, 10),      P(lato_10_bold, 10),      P(lato_10_italic, 10),  P(lato_10_bolditalic, 10),
    P(lato_12_regular, 12),      P(lato_12_bold, 12),      P(lato_12_italic, 12),  P(lato_12_bolditalic, 12),
    P(lato_14_regular, 14),      P(lato_14_bold, 14),      P(lato_14_italic, 14),  P(lato_14_bolditalic, 14),
    P(lato_16_regular, 16),      P(lato_16_bold, 16),      P(lato_16_italic, 16),  P(lato_16_bolditalic, 16),
    P(lato_17_regular, 17),      P(lato_17_bold, 17),      P(lato_17_italic, 17),  P(lato_17_bolditalic, 17),

    P(helvetica_10_regular, 10), P(helvetica_10_bold, 10), P(helvetica_10_italic, 10),
    P(helvetica_12_regular, 12), P(helvetica_12_bold, 12), P(helvetica_12_italic, 12),
    P(helvetica_14_regular, 14), P(helvetica_14_bold, 14), P(helvetica_14_italic, 14),
    P(helvetica_16_regular, 16), P(helvetica_16_bold, 16), P(helvetica_16_italic, 16),
    P(helvetica_17_regular, 17), P(helvetica_17_bold, 17), P(helvetica_17_italic, 17),

    P(verdana_10_regular, 10),   P(verdana_10_bold, 10),   P(verdana_10_italic, 10),
    P(verdana_12_regular, 12),   P(verdana_12_bold, 12),   P(verdana_12_italic, 12),
    P(verdana_14_regular, 14),   P(verdana_14_bold, 14),   P(verdana_14_italic, 14),
    P(verdana_16_regular, 16),   P(verdana_16_bold, 16),   P(verdana_16_italic, 16),
    P(verdana_17_regular, 17),   P(verdana_17_bold, 17),   P(verdana_17_italic, 17),
};

}  // namespace

int main(int argc, char** argv) {
  const char* outDir = argc > 1 ? argv[1] : ".";
  int ok = 0, fail = 0;
  for (const Row& r : kRows) {
    const std::string path = std::string(outDir) + "/" + r.name;
    FileSink sink;
    sink.f = std::fopen(path.c_str(), "wb");
    if (sink.f == nullptr) {
      std::printf("open failed: %s\n", path.c_str());
      ++fail;
      continue;
    }
    const bool exported = exportFontBlob(*r.fd, /*variant=*/0, r.sizePt, sink);
    std::fclose(sink.f);
    if (exported) {
      ++ok;
    } else {
      std::printf("export failed: %s\n", r.name);
      ++fail;
    }
  }
  std::printf("%d packs written to %s, %d failed\n", ok, outDir, fail);
  return fail ? 1 : 0;
}
