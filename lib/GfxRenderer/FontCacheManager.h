#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <map>
#include <string>

class FontDecompressor;

class FontCacheManager {
 public:
  explicit FontCacheManager(const std::map<int, EpdFontFamily>& fontMap);

  void setFontDecompressor(FontDecompressor* d);

  void clearCache();
  void prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F);
  void logStats(const char* label = "render");
  void resetStats();

  // Scan-mode API: called by GfxRenderer::drawText() during scan pass
  bool isScanning() const;
  void recordText(const char* text, int fontId, EpdFontFamily::Style style);

  // The FontDecompressor pointer, needed by GfxRenderer::getGlyphBitmap()
  FontDecompressor* getDecompressor() const { return fontDecompressor_; }

  // RAII scope for two-pass prewarm pattern
  class PrewarmScope {
   public:
    explicit PrewarmScope(FontCacheManager& manager);
    ~PrewarmScope();
    // requestedStyleMask intersects the scanned-style set (RFC #164 step 5);
    // the default 0x0F is the dormant Full behaviour (warm every used style).
    void endScanAndPrewarm(uint8_t requestedStyleMask = 0x0F);
    PrewarmScope(PrewarmScope&& other) noexcept;
    PrewarmScope& operator=(PrewarmScope&&) = delete;
    PrewarmScope(const PrewarmScope&) = delete;
    PrewarmScope& operator=(const PrewarmScope&) = delete;

   private:
    FontCacheManager* manager_;
    bool active_ = true;
  };
  PrewarmScope createPrewarmScope();

 private:
  const std::map<int, EpdFontFamily>& fontMap_;
  FontDecompressor* fontDecompressor_ = nullptr;

  enum class ScanMode : uint8_t { None, Scanning };
  ScanMode scanMode_ = ScanMode::None;
  // Distinct codepoints seen during the scan pass, deduplicated in recordText().
  // Replaces a full-page scanText_ std::string: prewarm only needs the set of
  // distinct glyphs, so we accumulate the distinct codepoints directly in a
  // fixed buffer (no per-page heap string that churned right before the big
  // prewarm allocations). Sized to the prewarm glyph cap (MAX_PAGE_GLYPHS=512).
  static constexpr uint16_t kMaxScanCodepoints = 512;
  uint32_t scanCodepoints_[kMaxScanCodepoints] = {};
  uint16_t scanCodepointCount_ = 0;
  uint32_t scanStyleCounts_[4] = {};
  int scanFontId_ = -1;
};
