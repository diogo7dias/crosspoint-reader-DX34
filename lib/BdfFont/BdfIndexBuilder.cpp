#include "BdfIndexBuilder.h"

#include <Arduino.h>
#include <HalStorage.h>

#include <algorithm>
#include <cstring>
#include <vector>

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

BuildIndexResult BdfIndexBuilder::buildIndex(const char* bdfPath, const char* idxPath,
                                             BuildProgressCallback progress, uint32_t progressEveryN) {
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

  // Two paths based on glyph count:
  //   - Small fonts (≤ kInMemorySortCap): collect into a vector, sort, then
  //     write. Tolerates BDFs that emit glyphs in non-ascending codepoint
  //     order (e.g. user-built BDFs that interleave Latin with punctuation).
  //   - Huge fonts (e.g. Unifont, 57 K glyphs): can't fit 16 B × N in heap,
  //     so we stream and require the input to be pre-sorted.
  static constexpr uint32_t kInMemorySortCap = 3000;  // 3000 × 16 B ≈ 48 KB

  uint32_t glyphsWritten = 0;
  const char* writeErr = nullptr;
  const uint32_t totalEstimate = header.glyphCount;
  const bool progressEnabled = progress && progressEveryN > 0;

  if (progressEnabled) progress(0, totalEstimate);

  BdfEnumResult enumRes;
  if (totalEstimate <= kInMemorySortCap) {
    // Collect-and-sort path.
    std::vector<IndexEntry> entries;
    entries.reserve(totalEstimate);
    auto cb = [&](const BdfGlyphMeta& g) -> bool {
      IndexEntry e{};
      e.codepoint = g.codepoint;
      e.bitmapOffset = g.bitmapOffset;
      e.bitmapW = g.bbxW;
      e.bitmapH = g.bbxH;
      e.advance = g.advance;
      e.bbxOffX = g.bbxOffX;
      e.bbxOffY = g.bbxOffY;
      entries.push_back(e);
      if (progressEnabled && (entries.size() % progressEveryN) == 0) {
        progress(static_cast<uint32_t>(entries.size()), totalEstimate);
      }
      return true;
    };
    enumRes = BdfParser::readAllGlyphs(bdfFile, cb);
    bdfFile.close();
    if (!enumRes.ok) {
      idxFile.close();
      Storage.remove(tmpPath);
      result.error = enumRes.error ? enumRes.error : "glyph enum failed";
      return result;
    }
    std::sort(entries.begin(), entries.end(),
              [](const IndexEntry& a, const IndexEntry& b) { return a.codepoint < b.codepoint; });
    // Drop duplicate codepoints (keep first occurrence) — binary search assumes
    // strictly ascending. BDFs with redundant glyphs still produce a valid idx.
    auto newEnd = std::unique(entries.begin(), entries.end(),
                              [](const IndexEntry& a, const IndexEntry& b) { return a.codepoint == b.codepoint; });
    entries.erase(newEnd, entries.end());
    for (const auto& e : entries) {
      if (!writePod(idxFile, &e, sizeof(e))) {
        writeErr = "write entry failed";
        break;
      }
      ++glyphsWritten;
    }
  } else {
    // Streaming path — must be pre-sorted.
    uint32_t lastCp = 0;
    bool first = true;
    auto cb = [&](const BdfGlyphMeta& g) -> bool {
      if (!first && g.codepoint <= lastCp) {
        writeErr = "BDF glyphs not sorted (too many glyphs to sort in RAM)";
        return false;
      }
      IndexEntry e{};
      e.codepoint = g.codepoint;
      e.bitmapOffset = g.bitmapOffset;
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
      if (progressEnabled && (glyphsWritten % progressEveryN) == 0) {
        progress(glyphsWritten, totalEstimate);
      }
      return true;
    };
    enumRes = BdfParser::readAllGlyphs(bdfFile, cb);
    bdfFile.close();
    if (!enumRes.ok) {
      idxFile.close();
      Storage.remove(tmpPath);
      result.error = enumRes.error ? enumRes.error : "glyph enum failed";
      return result;
    }
  }

  if (writeErr) {
    idxFile.close();
    Storage.remove(tmpPath);
    result.error = writeErr;
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
  // Final 100 % tick so UI can flip to the "installed" state before the
  // caller tears down the progress activity.
  if (progressEnabled) progress(glyphsWritten, totalEstimate);
  return result;
}

}  // namespace bdf
}  // namespace crosspoint
