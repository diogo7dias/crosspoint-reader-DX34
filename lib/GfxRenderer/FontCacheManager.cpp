#include "FontCacheManager.h"

#include <FontDecompressor.h>
#include <Logging.h>

#include <cstring>
#include <string>

namespace {

// Decode one UTF-8 codepoint at *pp and advance *pp past it. Defensive against
// truncated/invalid sequences (including an embedded '\0' where a continuation
// byte is expected): consumes a single byte and returns the lead byte so the
// caller's `while (*p)` loop terminates cleanly instead of over-reading.
uint32_t decodeUtf8(const unsigned char** pp) {
  const unsigned char* p = *pp;
  const unsigned char c = p[0];
  uint32_t cp;
  int len;
  if (c < 0x80) {
    *pp = p + 1;
    return c;
  } else if ((c & 0xE0) == 0xC0) {
    cp = c & 0x1F;
    len = 2;
  } else if ((c & 0xF0) == 0xE0) {
    cp = c & 0x0F;
    len = 3;
  } else if ((c & 0xF8) == 0xF0) {
    cp = c & 0x07;
    len = 4;
  } else {
    *pp = p + 1;  // invalid lead byte
    return c;
  }
  for (int i = 1; i < len; i++) {
    if ((p[i] & 0xC0) != 0x80) {  // truncated or embedded NUL
      *pp = p + 1;
      return c;
    }
    cp = (cp << 6) | (p[i] & 0x3F);
  }
  *pp = p + len;
  return cp;
}

// Append the UTF-8 encoding of `cp` to `out`.
void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

}  // namespace

FontCacheManager::FontCacheManager(const std::map<int, EpdFontFamily>& fontMap) : fontMap_(fontMap) {}

void FontCacheManager::setFontDecompressor(FontDecompressor* d) { fontDecompressor_ = d; }

void FontCacheManager::clearCache() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
}

void FontCacheManager::prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask) {
  if (!fontDecompressor_ || fontMap_.count(fontId) == 0) return;

  for (uint8_t i = 0; i < 4; i++) {
    if (!(styleMask & (1 << i))) continue;
    auto style = static_cast<EpdFontFamily::Style>(i);
    const EpdFontData* data = fontMap_.at(fontId).getData(style);
    if (!data || !data->groups) continue;
    int missed = fontDecompressor_->prewarmCache(data, utf8Text);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache: %d glyph(s) not cached for style %d", missed, i);
    }
  }
}

void FontCacheManager::logStats(const char* label) {
  if (fontDecompressor_) fontDecompressor_->logStats(label);
}

void FontCacheManager::resetStats() {
  if (fontDecompressor_) fontDecompressor_->resetStats();
}

bool FontCacheManager::isScanning() const { return scanMode_ == ScanMode::Scanning; }

void FontCacheManager::recordText(const char* text, int fontId, EpdFontFamily::Style style) {
  if (!text) return;
  if (scanFontId_ < 0) scanFontId_ = fontId;
  const uint8_t baseStyle = static_cast<uint8_t>(style) & 0x03;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  uint32_t cpCount = 0;
  while (*p) {
    const uint32_t cp = decodeUtf8(&p);
    cpCount++;
    // Deduplicate into the distinct-codepoint set (linear scan; the set is
    // bounded by kMaxScanCodepoints and a page has only tens-to-low-hundreds of
    // distinct codepoints, so this is cheap). Excess beyond the cap falls back
    // to on-demand glyph decode at render time, same as the old text-overflow
    // behaviour.
    bool found = false;
    for (uint16_t i = 0; i < scanCodepointCount_; i++) {
      if (scanCodepoints_[i] == cp) {
        found = true;
        break;
      }
    }
    if (!found && scanCodepointCount_ < kMaxScanCodepoints) {
      scanCodepoints_[scanCodepointCount_++] = cp;
    }
  }
  scanStyleCounts_[baseStyle] += cpCount;
}

// --- PrewarmScope implementation ---

FontCacheManager::PrewarmScope::PrewarmScope(FontCacheManager& manager) : manager_(&manager) {
  manager_->scanMode_ = ScanMode::Scanning;
  manager_->clearCache();
  manager_->resetStats();
  manager_->scanCodepointCount_ = 0;
  memset(manager_->scanStyleCounts_, 0, sizeof(manager_->scanStyleCounts_));
  manager_->scanFontId_ = -1;
}

void FontCacheManager::PrewarmScope::endScanAndPrewarm(uint8_t requestedStyleMask) {
  manager_->scanMode_ = ScanMode::None;
  if (manager_->scanCodepointCount_ == 0) return;

  // Build style bitmask from all styles that appeared during the scan
  uint8_t styleMask = 0;
  for (uint8_t i = 0; i < 4; i++) {
    if (manager_->scanStyleCounts_[i] > 0) styleMask |= (1 << i);
  }
  if (styleMask == 0) styleMask = 1;  // default to regular

  // RFC #164 step 5: intersect with the caller's requested mask (a degraded
  // render level warms fewer styles to save heap). The default 0x0F leaves the
  // scanned set untouched — the dormant Full behaviour. Never warm a style the
  // page never used, and always keep at least regular.
  styleMask &= (requestedStyleMask & 0x0F);
  if (styleMask == 0) styleMask = 1;

  // Re-encode the distinct codepoints to a compact UTF-8 string (only the
  // unique glyphs — a few hundred bytes at most, not the whole page) and reuse
  // the existing prewarmCache path. prewarmCache dedups again per style and
  // sorts by glyph index, so first-appearance order here is irrelevant.
  std::string utf8;
  utf8.reserve(static_cast<size_t>(manager_->scanCodepointCount_) * 3);
  for (uint16_t i = 0; i < manager_->scanCodepointCount_; i++) {
    appendUtf8(utf8, manager_->scanCodepoints_[i]);
  }

  manager_->prewarmCache(manager_->scanFontId_, utf8.c_str(), styleMask);

  manager_->scanCodepointCount_ = 0;
}

FontCacheManager::PrewarmScope::~PrewarmScope() {
  if (active_) {
    endScanAndPrewarm();  // no-op if already called (codepoint set is empty)
    manager_->clearCache();
  }
}

FontCacheManager::PrewarmScope::PrewarmScope(PrewarmScope&& other) noexcept
    : manager_(other.manager_), active_(other.active_) {
  other.active_ = false;
}

FontCacheManager::PrewarmScope FontCacheManager::createPrewarmScope() { return PrewarmScope(*this); }
