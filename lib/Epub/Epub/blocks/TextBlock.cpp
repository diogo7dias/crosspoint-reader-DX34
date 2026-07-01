#include "TextBlock.h"

#include <GfxRenderer.h>
#include <HeapGuard.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstring>  // memcpy — explicit; libstdc++ does not include it transitively like libc++
#include <new>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  // Validate iterator bounds before rendering
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size()) {
    LOG_ERR("TXB", "Render skipped: size mismatch (words=%u, xpos=%u, styles=%u)\n", (uint32_t)words.size(),
            (uint32_t)wordXpos.size(), (uint32_t)wordStyles.size());
    return;
  }

  for (size_t i = 0; i < words.size(); i++) {
    const int wordX = wordXpos[i] + x;
    const EpdFontFamily::Style currentStyle = wordStyles[i];
    renderer.drawTextSpaced(fontId, wordX, y, words[i].c_str(), blockStyle.letterSpacing, true, currentStyle);

    // Skip during the font-cache scan pass: getTextWidthSpaced below forces
    // glyph lookups that miss the warming cache and thrash the SD card, and the
    // underline is never drawn on a discarded scan-pass framebuffer. (Upstream #2237.)
    if (!renderer.isFontCacheScanning() && (currentStyle & EpdFontFamily::UNDERLINE) != 0) {
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

  // Pre-check heap before resize — std::vector::resize() under -fno-exceptions
  // aborts on bad_alloc. Two checks:
  //   1. Total free / 2 ceiling, to bound how much of the heap we're willing
  //      to commit to a single block's word table.
  //   2. Largest contiguous free block: the three vector::resize() calls below
  //      each demand one contiguous block. A fragmented heap with enough total
  //      free but no contiguous room is the realistic failure mode (Crash 1 in
  //      v2.3.9 testing: total free was healthy but largest had crumbled).
  {
    // words: 32 B std::string * wc, wordXpos: 2 B * wc, wordStyles: 1 B * wc.
    // Each is a SEPARATE contiguous allocation, so the largest one wins.
    const size_t wordsBytes = wc * sizeof(std::string);
    const size_t estimatedTotalBytes = wc * 20u;
#ifdef ESP_PLATFORM
    if (estimatedTotalBytes > esp_get_free_heap_size() / 2) {
      LOG_ERR("TXB", "Deserialization skipped: %u words (~%u bytes) would exceed safe heap limit", wc,
              (unsigned)estimatedTotalBytes);
      return nullptr;
    }
#endif
    if (!crosspoint::heap::canAllocateContiguous(wordsBytes)) {
      LOG_ERR("TXB", "Deserialization skipped: %u words (~%u B contiguous) largest=%u", wc, (unsigned)wordsBytes,
              (unsigned)crosspoint::heap::largestFreeBlockBytes());
      return nullptr;
    }
  }

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
