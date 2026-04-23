#include "BdfIndexBuilder.h"

#include <Arduino.h>
#include <HalStorage.h>

#include <cstring>

#include "BdfIndex.h"
#include "BdfParser.h"

namespace crosspoint {
namespace bdf {

namespace {

bool writePod(HalFile& f, const void* data, size_t len) {
  return f.write(data, len) == len;
}

void buildTmpPath(const char* finalPath, char* out, size_t outSize) {
  // <finalPath>.tmp
  std::snprintf(out, outSize, "%s.tmp", finalPath);
}

}  // namespace

BuildIndexResult BdfIndexBuilder::buildIndex(const char* bdfPath, const char* idxPath) {
  BuildIndexResult result;
  if (!bdfPath || !idxPath) {
    result.error = "null path";
    return result;
  }

  const uint32_t startMs = millis();

  HalFile bdfFile = Storage.open(bdfPath);
  if (!bdfFile) {
    result.error = "cannot open BDF";
    return result;
  }

  // Parse header to populate font-wide metrics (and to position the cursor at
  // the first STARTCHAR).
  const BdfHeader header = BdfParser::readHeader(bdfFile);
  if (!header.ok) {
    bdfFile.close();
    result.error = header.error ? header.error : "header parse failed";
    return result;
  }
  // The header parser stops AT the first STARTCHAR line — meaning that line
  // has already been consumed. Rewind to the start and re-scan past header
  // so the glyph enumerator captures STARTCHAR offsets correctly.
  if (!bdfFile.seekSet(0)) {
    bdfFile.close();
    result.error = "rewind failed";
    return result;
  }
  // Re-skip header without using readHeader again (faster: just walk lines
  // until we see STARTCHAR; the header has been validated already). But we
  // also need the file position right BEFORE that STARTCHAR line so the
  // enumerator can record it as the first glyph offset.
  // Easiest: leave the cursor at 0 and let readAllGlyphs walk from there —
  // the leading non-glyph lines (STARTFONT, FONT, ...) are no-ops in its
  // state machine (it ignores everything before STARTCHAR).

  char tmpPath[320];
  buildTmpPath(idxPath, tmpPath, sizeof(tmpPath));
  Storage.remove(tmpPath);  // clean any stale .tmp from a prior aborted build

  HalFile idxFile;
  if (!Storage.openFileForWrite("BIB", tmpPath, idxFile)) {
    bdfFile.close();
    result.error = "cannot open .idx.tmp for write";
    return result;
  }

  // Write a placeholder header. glyphCount is fixed up at the end.
  IndexHeader hdr{};
  hdr.magic = kBdfIndexMagic;
  hdr.version = kBdfIndexVersion;
  hdr.glyphCount = 0;
  hdr.fontBbxW = static_cast<int8_t>(header.bbxW);
  hdr.fontBbxH = static_cast<int8_t>(header.bbxH);
  hdr.fontAscent = static_cast<int8_t>(header.ascent);
  hdr.fontDescent = static_cast<int8_t>(header.descent);
  if (!writePod(idxFile, &hdr, sizeof(hdr))) {
    idxFile.close();
    Storage.remove(tmpPath);
    bdfFile.close();
    result.error = "write header failed";
    return result;
  }

  // Enumeration callback: write one IndexEntry per glyph. Reject out-of-order
  // codepoints (binary search in the runtime requires sorted entries).
  uint32_t glyphsWritten = 0;
  uint32_t lastCp = 0;
  bool first = true;
  const char* writeErr = nullptr;

  auto cb = [&](const BdfGlyphMeta& g) -> bool {
    if (!first && g.codepoint <= lastCp) {
      writeErr = "BDF glyphs not sorted by codepoint";
      return false;  // abort enumeration
    }
    IndexEntry e{};
    e.codepoint = g.codepoint;
    e.bdfOffset = g.bdfOffset;
    e.bitmapW = g.bbxW;
    e.bitmapH = g.bbxH;
    e.advance = g.advance;
    e.bbxOffX = g.bbxOffX;
    e.bbxOffY = g.bbxOffY;
    if (!writePod(idxFile, &e, sizeof(e))) {
      writeErr = "write entry failed";
      return false;
    }
    ++glyphsWritten;
    lastCp = g.codepoint;
    first = false;
    return true;
  };

  const BdfEnumResult enumRes = BdfParser::readAllGlyphs(bdfFile, cb);
  bdfFile.close();

  if (writeErr) {
    idxFile.close();
    Storage.remove(tmpPath);
    result.error = writeErr;
    return result;
  }
  if (!enumRes.ok) {
    idxFile.close();
    Storage.remove(tmpPath);
    result.error = enumRes.error ? enumRes.error : "glyph enum failed";
    return result;
  }

  // Patch the header glyphCount in place.
  if (!idxFile.seekSet(offsetof(IndexHeader, glyphCount))) {
    idxFile.close();
    Storage.remove(tmpPath);
    result.error = "seek-back to header failed";
    return result;
  }
  if (!writePod(idxFile, &glyphsWritten, sizeof(glyphsWritten))) {
    idxFile.close();
    Storage.remove(tmpPath);
    result.error = "patch glyphCount failed";
    return result;
  }
  idxFile.flush();
  idxFile.close();

  Storage.remove(idxPath);
  if (!Storage.rename(tmpPath, idxPath)) {
    Storage.remove(tmpPath);
    result.error = "rename .tmp -> .idx failed";
    return result;
  }

  result.glyphsWritten = glyphsWritten;
  result.glyphsSkipped = enumRes.glyphsSkipped;
  result.parseTimeMs = millis() - startMs;
  result.ok = true;
  return result;
}

}  // namespace bdf
}  // namespace crosspoint
