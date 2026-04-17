#include "TextBlock.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#include <new>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

// Returns the byte offset into a UTF-8 string at which the bionic bold prefix
// ends.  The prefix covers roughly half the visible codepoints (rounded up).
// Leading em-space (U+2003) is skipped so the prefix starts at the first real
// character.
static size_t bionicSplitOffset(const char* s, const size_t len) {
  // Count total codepoints (skip leading em-space)
  size_t pos = 0;
  // Skip leading em-space (\xe2\x80\x83 = U+2003)
  size_t leadingBytes = 0;
  if (len >= 3 && static_cast<uint8_t>(s[0]) == 0xE2 && static_cast<uint8_t>(s[1]) == 0x80 &&
      static_cast<uint8_t>(s[2]) == 0x83) {
    leadingBytes = 3;
    pos = 3;
  }

  // Count visible codepoints
  size_t cpCount = 0;
  size_t tmpPos = pos;
  while (tmpPos < len) {
    const uint8_t b = static_cast<uint8_t>(s[tmpPos]);
    if (b < 0x80)
      tmpPos += 1;
    else if ((b & 0xE0) == 0xC0)
      tmpPos += 2;
    else if ((b & 0xF0) == 0xE0)
      tmpPos += 3;
    else
      tmpPos += 4;
    cpCount++;
  }

  if (cpCount <= 1) return len;  // 0-1 char words: whole word bold

  // Graduated bold prefix: shorter words get less bold for a subtler effect.
  //   2-3 chars: bold first letter only
  //   4-6 chars: bold first 2 chars (~40%)
  //   7+  chars: bold first ~40% (rounded up)
  size_t prefixCps;
  if (cpCount <= 3) {
    prefixCps = 1;
  } else if (cpCount <= 6) {
    prefixCps = 2;
  } else {
    prefixCps = (cpCount * 2 + 4) / 5;  // ceil(cpCount * 0.4)
  }

  // Advance pos by prefixCps codepoints to find byte offset
  size_t counted = 0;
  size_t splitPos = pos;
  while (splitPos < len && counted < prefixCps) {
    const uint8_t b = static_cast<uint8_t>(s[splitPos]);
    if (b < 0x80)
      splitPos += 1;
    else if ((b & 0xE0) == 0xC0)
      splitPos += 2;
    else if ((b & 0xF0) == 0xE0)
      splitPos += 3;
    else
      splitPos += 4;
    counted++;
  }

  return splitPos;
}

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  // Validate iterator bounds before rendering
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size()) {
    LOG_ERR("TXB", "Render skipped: size mismatch (words=%u, xpos=%u, styles=%u)\n", (uint32_t)words.size(),
            (uint32_t)wordXpos.size(), (uint32_t)wordStyles.size());
    return;
  }

  const bool bionicMode = renderer.getTextRenderStyle() == 2;

  for (size_t i = 0; i < words.size(); i++) {
    const int wordX = wordXpos[i] + x;
    const EpdFontFamily::Style currentStyle = wordStyles[i];

    // Bionic reading: bold the first half of each word (skip if already bold)
    if (bionicMode && !(currentStyle & EpdFontFamily::BOLD) && !words[i].empty()) {
      const char* text = words[i].c_str();
      const size_t len = words[i].size();
      const size_t splitAt = bionicSplitOffset(text, len);

      if (splitAt >= len) {
        // Whole word bold
        const auto boldStyle = static_cast<EpdFontFamily::Style>(currentStyle | EpdFontFamily::BOLD);
        renderer.drawTextSpaced(fontId, wordX, y, text, blockStyle.letterSpacing, true, boldStyle);
      } else {
        // Draw bold prefix
        char prefixBuf[64];
        const size_t copyLen = (splitAt < sizeof(prefixBuf) - 1) ? splitAt : sizeof(prefixBuf) - 1;
        memcpy(prefixBuf, text, copyLen);
        prefixBuf[copyLen] = '\0';

        const auto boldStyle = static_cast<EpdFontFamily::Style>(currentStyle | EpdFontFamily::BOLD);
        renderer.drawTextSpaced(fontId, wordX, y, prefixBuf, blockStyle.letterSpacing, true, boldStyle);
        const int prefixAdvance =
            renderer.getTextAdvanceXSpaced(fontId, prefixBuf, blockStyle.letterSpacing, boldStyle);

        // Draw regular suffix
        renderer.drawTextSpaced(fontId, wordX + prefixAdvance, y, text + splitAt, blockStyle.letterSpacing, true,
                                currentStyle);
      }
    } else {
      renderer.drawTextSpaced(fontId, wordX, y, words[i].c_str(), blockStyle.letterSpacing, true, currentStyle);
    }

    if ((currentStyle & EpdFontFamily::UNDERLINE) != 0) {
      const std::string& w = words[i];
      const int fullWordWidth = renderer.getTextWidthSpaced(fontId, w.c_str(), blockStyle.letterSpacing, currentStyle);
      // y is the top of the text line; add ascender to reach baseline, then offset 2px below
      const int underlineY = y + renderer.getFontAscenderSize(fontId) + 2;

      int startX = wordX;
      int underlineWidth = fullWordWidth;

      // if word starts with em-space ("\xe2\x80\x83"), account for the additional indent before drawing the line
      if (w.size() >= 3 && static_cast<uint8_t>(w[0]) == 0xE2 && static_cast<uint8_t>(w[1]) == 0x80 &&
          static_cast<uint8_t>(w[2]) == 0x83) {
        const char* visiblePtr = w.c_str() + 3;
        const int prefixWidth =
            renderer.getTextAdvanceXSpaced(fontId, "\xe2\x80\x83", blockStyle.letterSpacing, currentStyle);
        const int visibleWidth =
            renderer.getTextWidthSpaced(fontId, visiblePtr, blockStyle.letterSpacing, currentStyle);
        startX = wordX + prefixWidth;
        underlineWidth = visibleWidth;
      }

      renderer.drawLine(startX, underlineY, startX + underlineWidth, underlineY, true);
    }
  }
}

bool TextBlock::serialize(FsFile& file) const {
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size()) {
    LOG_ERR("TXB", "Serialization failed: size mismatch (words=%u, xpos=%u, styles=%u)\n", words.size(),
            wordXpos.size(), wordStyles.size());
    return false;
  }

  // Word data
  serialization::writePod(file, static_cast<uint16_t>(words.size()));
  for (const auto& w : words) serialization::writeString(file, w);
  for (auto x : wordXpos) serialization::writePod(file, x);
  for (auto s : wordStyles) serialization::writePod(file, s);

  // Style (alignment + margins/padding/indent)
  serialization::writePod(file, blockStyle.alignment);
  serialization::writePod(file, blockStyle.textAlignDefined);
  serialization::writePod(file, blockStyle.marginTop);
  serialization::writePod(file, blockStyle.marginBottom);
  serialization::writePod(file, blockStyle.marginLeft);
  serialization::writePod(file, blockStyle.marginRight);
  serialization::writePod(file, blockStyle.paddingTop);
  serialization::writePod(file, blockStyle.paddingBottom);
  serialization::writePod(file, blockStyle.paddingLeft);
  serialization::writePod(file, blockStyle.paddingRight);
  serialization::writePod(file, blockStyle.textIndent);
  serialization::writePod(file, blockStyle.textIndentDefined);
  serialization::writePod(file, blockStyle.letterSpacing);
  serialization::writePod(file, blockStyle.wordSpacing);
  serialization::writePod(file, blockStyle.lineHeightPercent);
  serialization::writePod(file, blockStyle.letterSpacingDefined);
  serialization::writePod(file, blockStyle.wordSpacingDefined);
  serialization::writePod(file, blockStyle.lineHeightDefined);

  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(FsFile& file) {
  uint16_t wc;
  std::vector<std::string> words;
  std::vector<int16_t> wordXpos;
  std::vector<EpdFontFamily::Style> wordStyles;
  BlockStyle blockStyle;

  // Word count
  serialization::readPod(file, wc);

  // Sanity check: prevent allocation of unreasonably large vectors (max 10000 words per block)
  if (wc > 10000) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }

  // Pre-check heap before resize — on ESP32, vector::resize() calls abort() on OOM
#ifdef ESP_PLATFORM
  {
    // Estimate: each word averages ~12 bytes string + 2 bytes xpos + 1 byte style
    const size_t estimatedBytes = wc * 20u;
    if (estimatedBytes > esp_get_free_heap_size() / 2) {
      LOG_ERR("TXB", "Deserialization skipped: %u words (~%u bytes) would exceed safe heap limit", wc,
              (unsigned)estimatedBytes);
      return nullptr;
    }
  }
#endif

  // Word data
  words.resize(wc);
  wordXpos.resize(wc);
  wordStyles.resize(wc);
  for (auto& w : words) serialization::readString(file, w);
  for (auto& x : wordXpos) serialization::readPod(file, x);
  for (auto& s : wordStyles) serialization::readPod(file, s);

  // Style (alignment + margins/padding/indent)
  serialization::readPod(file, blockStyle.alignment);
  serialization::readPod(file, blockStyle.textAlignDefined);
  serialization::readPod(file, blockStyle.marginTop);
  serialization::readPod(file, blockStyle.marginBottom);
  serialization::readPod(file, blockStyle.marginLeft);
  serialization::readPod(file, blockStyle.marginRight);
  serialization::readPod(file, blockStyle.paddingTop);
  serialization::readPod(file, blockStyle.paddingBottom);
  serialization::readPod(file, blockStyle.paddingLeft);
  serialization::readPod(file, blockStyle.paddingRight);
  serialization::readPod(file, blockStyle.textIndent);
  serialization::readPod(file, blockStyle.textIndentDefined);
  serialization::readPod(file, blockStyle.letterSpacing);
  serialization::readPod(file, blockStyle.wordSpacing);
  serialization::readPod(file, blockStyle.lineHeightPercent);
  serialization::readPod(file, blockStyle.letterSpacingDefined);
  serialization::readPod(file, blockStyle.wordSpacingDefined);
  serialization::readPod(file, blockStyle.lineHeightDefined);

  auto* tb = new (std::nothrow) TextBlock(std::move(words), std::move(wordXpos), std::move(wordStyles), blockStyle);
  if (!tb) {
    LOG_ERR("TXB", "OOM: TextBlock");
    return nullptr;
  }
  return std::unique_ptr<TextBlock>(tb);
}
