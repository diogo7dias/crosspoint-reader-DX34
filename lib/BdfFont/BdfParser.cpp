#include "BdfParser.h"

#include <Arduino.h>
#include <esp_task_wdt.h>

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace crosspoint {
namespace bdf {

namespace {

constexpr size_t kLineBuf = 192;

// Reads a single line from `in` into `buf` (NUL-terminated, trimmed of CR).
// Lines longer than the buffer are truncated; the remainder is discarded so
// the next call still starts at a line boundary. Returns the number of bytes
// stored, or -1 on EOF before any byte was read.
int readLine(HalFile& in, char* buf, size_t bufSize) {
  if (bufSize == 0) return -1;
  size_t len = 0;
  bool sawAny = false;
  while (true) {
    const int c = in.read();
    if (c < 0) {
      // EOF. If we already collected bytes, return what we have.
      if (!sawAny) return -1;
      break;
    }
    sawAny = true;
    if (c == '\n') break;
    if (c == '\r') continue;
    if (len + 1 < bufSize) {
      buf[len++] = static_cast<char>(c);
    }
    // else: silently drop overflow; we still want to land on the next \n.
  }
  buf[len] = '\0';
  return static_cast<int>(len);
}

// Returns true when `line` starts with `token` AND the next char is a space
// or end-of-string (so "STARTCHAR" does not match "STARTFONT").
bool startsWithToken(const char* line, const char* token) {
  const size_t tlen = std::strlen(token);
  if (std::strncmp(line, token, tlen) != 0) return false;
  const char next = line[tlen];
  return next == '\0' || next == ' ' || next == '\t';
}

// Skips whitespace, then parses a signed decimal int from `*p`. Advances `*p`
// past the parsed digits. Returns false if no digits were found.
bool parseInt(const char** p, long* out) {
  while (**p == ' ' || **p == '\t') ++(*p);
  char* end = nullptr;
  const long v = std::strtol(*p, &end, 10);
  if (end == *p) return false;
  *p = end;
  *out = v;
  return true;
}

}  // namespace

BdfHeader BdfParser::readHeader(HalFile& in, const size_t wdtTickEvery) {
  BdfHeader h;
  char line[kLineBuf];

  bool sawStartFont = false;
  bool sawCharsCount = false;
  size_t lineNum = 0;

  while (true) {
    const int n = readLine(in, line, sizeof(line));
    if (n < 0) {
      // EOF before STARTCHAR. Header is complete only if we got the bare
      // minimum (STARTFONT + CHARS N) — otherwise the file is malformed.
      if (sawStartFont && sawCharsCount) {
        h.ok = true;
      } else {
        h.error = "unexpected EOF in header";
      }
      return h;
    }

    if (++lineNum % wdtTickEvery == 0) {
      esp_task_wdt_reset();
      yield();
    }

    if (n == 0) continue;  // blank line

    if (startsWithToken(line, "STARTFONT")) {
      sawStartFont = true;
      continue;
    }
    if (!sawStartFont) {
      h.error = "missing STARTFONT";
      return h;
    }

    if (startsWithToken(line, "STARTCHAR")) {
      // First glyph reached. Header parse is complete iff CHARS was seen.
      if (!sawCharsCount) {
        h.error = "missing CHARS before first STARTCHAR";
        return h;
      }
      h.ok = true;
      return h;
    }

    if (startsWithToken(line, "CHARS")) {
      const char* p = line + std::strlen("CHARS");
      long v = 0;
      if (!parseInt(&p, &v) || v < 0) {
        h.error = "bad CHARS count";
        return h;
      }
      h.glyphCount = static_cast<uint32_t>(v);
      sawCharsCount = true;
      continue;
    }

    if (startsWithToken(line, "FONTBOUNDINGBOX")) {
      const char* p = line + std::strlen("FONTBOUNDINGBOX");
      long w = 0, ht = 0, ox = 0, oy = 0;
      if (!parseInt(&p, &w) || !parseInt(&p, &ht) || !parseInt(&p, &ox) || !parseInt(&p, &oy)) {
        h.error = "bad FONTBOUNDINGBOX";
        return h;
      }
      h.bbxW = static_cast<int16_t>(w);
      h.bbxH = static_cast<int16_t>(ht);
      h.bbxOffX = static_cast<int16_t>(ox);
      h.bbxOffY = static_cast<int16_t>(oy);
      continue;
    }

    if (startsWithToken(line, "SIZE")) {
      const char* p = line + std::strlen("SIZE");
      long pt = 0;
      if (parseInt(&p, &pt) && pt > 0 && pt < 65536) {
        h.pointSize = static_cast<uint16_t>(pt);
      }
      continue;
    }

    if (startsWithToken(line, "FONT_ASCENT")) {
      const char* p = line + std::strlen("FONT_ASCENT");
      long v = 0;
      if (parseInt(&p, &v)) h.ascent = static_cast<int16_t>(v);
      continue;
    }

    if (startsWithToken(line, "FONT_DESCENT")) {
      const char* p = line + std::strlen("FONT_DESCENT");
      long v = 0;
      if (parseInt(&p, &v)) h.descent = static_cast<int16_t>(v);
      continue;
    }
    // All other lines (COMMENT, FONT, STARTPROPERTIES, ENDPROPERTIES, ...)
    // are ignored.
  }
}

BdfEnumResult BdfParser::readAllGlyphs(HalFile& in, const BdfGlyphCallback& cb, const size_t wdtTickEvery) {
  BdfEnumResult result;
  char line[kLineBuf];
  size_t lineNum = 0;

  // Per-glyph mutable state, reset between STARTCHARs.
  bool inGlyph = false;
  bool inBitmap = false;
  uint32_t glyphStartOffset = 0;
  uint32_t bitmapOffset = 0;  // file position of first hex row, set at BITMAP
  long encoding = -1;
  long bbxW = 0, bbxH = 0, bbxOX = 0, bbxOY = 0;
  long dwidthX = 0;
  bool sawEncoding = false;
  bool sawBbx = false;

  auto resetGlyph = [&]() {
    inGlyph = false;
    inBitmap = false;
    bitmapOffset = 0;
    encoding = -1;
    bbxW = bbxH = bbxOX = bbxOY = 0;
    dwidthX = 0;
    sawEncoding = false;
    sawBbx = false;
  };

  while (true) {
    const uint32_t lineStartOffset = static_cast<uint32_t>(in.position());
    const int n = readLine(in, line, sizeof(line));
    if (n < 0) {
      // EOF. ENDFONT may or may not have been seen; either way the file is
      // exhausted. If we were mid-glyph that is malformed.
      if (inGlyph) {
        result.error = "EOF inside glyph";
        return result;
      }
      result.ok = true;
      return result;
    }

    if (++lineNum % wdtTickEvery == 0) {
      esp_task_wdt_reset();
      yield();
    }

    if (n == 0) continue;

    if (inBitmap) {
      // Skip every line until we hit ENDCHAR. Bitmap rows are pure hex; we
      // do not need them for index building.
      if (startsWithToken(line, "ENDCHAR")) {
        if (sawEncoding && sawBbx && encoding >= 0) {
          BdfGlyphMeta meta;
          meta.codepoint = static_cast<uint32_t>(encoding);
          meta.bdfOffset = glyphStartOffset;
          meta.bitmapOffset = bitmapOffset;
          meta.bbxW = static_cast<uint8_t>(bbxW < 0 ? 0 : (bbxW > 255 ? 255 : bbxW));
          meta.bbxH = static_cast<uint8_t>(bbxH < 0 ? 0 : (bbxH > 255 ? 255 : bbxH));
          meta.bbxOffX = static_cast<int8_t>(bbxOX < -128 ? -128 : (bbxOX > 127 ? 127 : bbxOX));
          meta.bbxOffY = static_cast<int8_t>(bbxOY < -128 ? -128 : (bbxOY > 127 ? 127 : bbxOY));
          meta.advance = static_cast<uint8_t>(dwidthX < 0 ? 0 : (dwidthX > 255 ? 255 : dwidthX));
          ++result.glyphsYielded;
          if (!cb(meta)) {
            result.ok = true;
            return result;
          }
        } else {
          ++result.glyphsSkipped;
        }
        resetGlyph();
      }
      continue;
    }

    if (startsWithToken(line, "STARTCHAR")) {
      if (inGlyph) {
        // Nested STARTCHAR — malformed. Treat as reset.
        ++result.glyphsSkipped;
        resetGlyph();
      }
      inGlyph = true;
      glyphStartOffset = lineStartOffset;
      continue;
    }

    if (!inGlyph) {
      if (startsWithToken(line, "ENDFONT")) {
        result.ok = true;
        return result;
      }
      // Outside any glyph and not ENDFONT — skip silently (could be tail
      // comments or whitespace).
      continue;
    }

    if (startsWithToken(line, "ENCODING")) {
      const char* p = line + std::strlen("ENCODING");
      if (parseInt(&p, &encoding)) sawEncoding = true;
      continue;
    }
    if (startsWithToken(line, "BBX")) {
      const char* p = line + std::strlen("BBX");
      if (parseInt(&p, &bbxW) && parseInt(&p, &bbxH) && parseInt(&p, &bbxOX) && parseInt(&p, &bbxOY)) {
        sawBbx = true;
      }
      continue;
    }
    if (startsWithToken(line, "DWIDTH")) {
      const char* p = line + std::strlen("DWIDTH");
      parseInt(&p, &dwidthX);  // ignore Y component (vertical advance)
      continue;
    }
    if (startsWithToken(line, "BITMAP")) {
      inBitmap = true;
      // readLine has already consumed the BITMAP line AND its trailing \n,
      // so the file cursor sits exactly at the first byte of the first hex
      // row. That is the offset decodeBitmap() will seek to at runtime.
      bitmapOffset = static_cast<uint32_t>(in.position());
      continue;
    }
    if (startsWithToken(line, "ENDCHAR")) {
      // ENDCHAR without BITMAP is malformed but tolerated — treat as a
      // bitmap-less glyph (still has metrics). bitmapOffset stays 0 — the
      // runtime decoder interprets 0 as "no bitmap" and skips the read.
      if (sawEncoding && sawBbx && encoding >= 0) {
        BdfGlyphMeta meta;
        meta.codepoint = static_cast<uint32_t>(encoding);
        meta.bdfOffset = glyphStartOffset;
        meta.bitmapOffset = bitmapOffset;
        meta.bbxW = static_cast<uint8_t>(bbxW < 0 ? 0 : (bbxW > 255 ? 255 : bbxW));
        meta.bbxH = static_cast<uint8_t>(bbxH < 0 ? 0 : (bbxH > 255 ? 255 : bbxH));
        meta.bbxOffX = static_cast<int8_t>(bbxOX < -128 ? -128 : (bbxOX > 127 ? 127 : bbxOX));
        meta.bbxOffY = static_cast<int8_t>(bbxOY < -128 ? -128 : (bbxOY > 127 ? 127 : bbxOY));
        meta.advance = static_cast<uint8_t>(dwidthX < 0 ? 0 : (dwidthX > 255 ? 255 : dwidthX));
        ++result.glyphsYielded;
        if (!cb(meta)) {
          result.ok = true;
          return result;
        }
      } else {
        ++result.glyphsSkipped;
      }
      resetGlyph();
      continue;
    }
    // Any other lines inside a glyph (SWIDTH, etc.) are ignored.
  }
}

}  // namespace bdf
}  // namespace crosspoint
