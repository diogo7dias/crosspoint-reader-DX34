#include "CssParser.h"

#include <Arduino.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace {

// Stack-allocated string buffer to avoid heap reallocations during parsing
// Provides string-like interface with fixed capacity
struct StackBuffer {
  static constexpr size_t CAPACITY = 1024;
  char data[CAPACITY];
  size_t len = 0;

  void push_back(char c) {
    if (len < CAPACITY - 1) {
      data[len++] = c;
    }
  }

  void clear() { len = 0; }
  bool empty() const { return len == 0; }
  size_t size() const { return len; }

  // Get string view of current content (zero-copy)
  std::string_view view() const { return std::string_view(data, len); }

  // Convert to string for passing to functions (single allocation)
  std::string str() const { return std::string(data, len); }
};

// Buffer size for reading CSS files
constexpr size_t READ_BUFFER_SIZE = 512;

// Maximum number of CSS rules to store in the selector map
// Prevents unbounded memory growth from pathological CSS files
constexpr size_t MAX_RULES = 1500;

// Minimum free heap required to apply CSS during rendering
// If below this threshold, we skip CSS to avoid display artifacts.
constexpr size_t MIN_FREE_HEAP_FOR_CSS = 48 * 1024;

// Maximum length for a single selector string
// Prevents parsing of extremely long or malformed selectors
constexpr size_t MAX_SELECTOR_LENGTH = 256;

// Check if character is CSS whitespace
bool isCssWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

}  // anonymous namespace

// String utilities implementation

std::string CssParser::normalized(const std::string& s) {
  std::string result;
  result.reserve(s.size());

  bool inSpace = true;  // Start true to skip leading space
  for (const char c : s) {
    if (isCssWhitespace(c)) {
      if (!inSpace) {
        result.push_back(' ');
        inSpace = true;
      }
    } else {
      result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      inSpace = false;
    }
  }

  // Remove trailing space
  if (!result.empty() && result.back() == ' ') {
    result.pop_back();
  }
  return result;
}

void CssParser::normalizedInto(const std::string& s, std::string& out) {
  out.clear();
  out.reserve(s.size());

  bool inSpace = true;  // Start true to skip leading space
  for (const char c : s) {
    if (isCssWhitespace(c)) {
      if (!inSpace) {
        out.push_back(' ');
        inSpace = true;
      }
    } else {
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      inSpace = false;
    }
  }

  if (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
}

std::vector<std::string> CssParser::splitOnChar(const std::string& s, const char delimiter) {
  std::vector<std::string> parts;
  size_t start = 0;

  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == delimiter) {
      std::string part = s.substr(start, i - start);
      std::string trimmed = normalized(part);
      if (!trimmed.empty()) {
        parts.push_back(trimmed);
      }
      start = i + 1;
    }
  }
  return parts;
}

std::vector<std::string> CssParser::splitWhitespace(const std::string& s) {
  std::vector<std::string> parts;
  size_t start = 0;
  bool inWord = false;

  for (size_t i = 0; i <= s.size(); ++i) {
    const bool isSpace = i == s.size() || isCssWhitespace(s[i]);
    if (isSpace && inWord) {
      parts.push_back(s.substr(start, i - start));
      inWord = false;
    } else if (!isSpace && !inWord) {
      start = i;
      inWord = true;
    }
  }
  return parts;
}

// Property value interpreters

CssTextAlign CssParser::interpretAlignment(const std::string& val) {
  const std::string v = normalized(val);

  if (v == "left" || v == "start") return CssTextAlign::Left;
  if (v == "right" || v == "end") return CssTextAlign::Right;
  if (v == "center") return CssTextAlign::Center;
  if (v == "justify") return CssTextAlign::Justify;

  return CssTextAlign::Left;
}

CssFontStyle CssParser::interpretFontStyle(const std::string& val) {
  const std::string v = normalized(val);

  if (v == "italic" || v == "oblique") return CssFontStyle::Italic;
  return CssFontStyle::Normal;
}

CssFontWeight CssParser::interpretFontWeight(const std::string& val) {
  const std::string v = normalized(val);

  // Named values
  if (v == "bold" || v == "bolder") return CssFontWeight::Bold;
  if (v == "normal" || v == "lighter") return CssFontWeight::Normal;

  // Numeric values: 100-900
  // CSS spec: 400 = normal, 700 = bold
  // We use: 0-400 = normal, 700+ = bold, 500-600 = normal (conservative)
  char* endPtr = nullptr;
  const long numericWeight = std::strtol(v.c_str(), &endPtr, 10);

  // If we parsed a number and consumed the whole string
  if (endPtr != v.c_str() && *endPtr == '\0') {
    return numericWeight >= 700 ? CssFontWeight::Bold : CssFontWeight::Normal;
  }

  return CssFontWeight::Normal;
}

CssTextDecoration CssParser::interpretDecoration(const std::string& val) {
  const std::string v = normalized(val);

  // text-decoration can have multiple space-separated values
  if (v.find("underline") != std::string::npos) {
    return CssTextDecoration::Underline;
  }
  return CssTextDecoration::None;
}

CssLength CssParser::interpretLength(const std::string& val) {
  const std::string v = normalized(val);
  if (v.empty()) return CssLength{};

  // Find where the number ends
  size_t unitStart = v.size();
  for (size_t i = 0; i < v.size(); ++i) {
    const char c = v[i];
    if (!std::isdigit(c) && c != '.' && c != '-' && c != '+') {
      unitStart = i;
      break;
    }
  }

  const std::string numPart = v.substr(0, unitStart);
  const std::string unitPart = v.substr(unitStart);

  // Parse numeric value
  char* endPtr = nullptr;
  const float numericValue = std::strtof(numPart.c_str(), &endPtr);
  if (endPtr == numPart.c_str()) return CssLength{};  // No number parsed

  // Determine unit type (preserve for deferred resolution)
  auto unit = CssUnit::Pixels;
  if (unitPart == "em") {
    unit = CssUnit::Em;
  } else if (unitPart == "rem") {
    unit = CssUnit::Rem;
  } else if (unitPart == "pt") {
    unit = CssUnit::Points;
  } else if (unitPart == "%") {
    unit = CssUnit::Percent;
  }
  // px and unitless default to Pixels

  return CssLength{numericValue, unit};
}
// Declaration parsing

void CssParser::parseDeclarationIntoStyle(const std::string& decl, CssStyle& style, std::string& propNameBuf,
                                          std::string& propValueBuf) {
  const size_t colonPos = decl.find(':');
  if (colonPos == std::string::npos || colonPos == 0) return;

  normalizedInto(decl.substr(0, colonPos), propNameBuf);
  normalizedInto(decl.substr(colonPos + 1), propValueBuf);

  if (propNameBuf.empty() || propValueBuf.empty()) return;

  if (propNameBuf == "text-align") {
    style.textAlign = interpretAlignment(propValueBuf);
    style.defined.textAlign = 1;
  } else if (propNameBuf == "font-style") {
    style.fontStyle = interpretFontStyle(propValueBuf);
    style.defined.fontStyle = 1;
  } else if (propNameBuf == "font-weight") {
    style.fontWeight = interpretFontWeight(propValueBuf);
    style.defined.fontWeight = 1;
  } else if (propNameBuf == "text-decoration" || propNameBuf == "text-decoration-line") {
    style.textDecoration = interpretDecoration(propValueBuf);
    style.defined.textDecoration = 1;
  } else if (propNameBuf == "text-indent") {
    style.textIndent = interpretLength(propValueBuf);
    style.defined.textIndent = 1;
  } else if (propNameBuf == "line-height") {
    if (propValueBuf == "normal") {
      style.lineHeight = CssLength(1.2f, CssUnit::Em);
    } else {
      CssLength interpreted = interpretLength(propValueBuf);
      if (interpreted.unit == CssUnit::Pixels && propValueBuf.find_first_not_of("+-0123456789.") == std::string::npos) {
        interpreted.unit = CssUnit::Em;
      }
      style.lineHeight = interpreted;
    }
    style.defined.lineHeight = 1;
  } else if (propNameBuf == "letter-spacing") {
    if (propValueBuf == "normal") {
      style.letterSpacing = CssLength{};
    } else {
      style.letterSpacing = interpretLength(propValueBuf);
    }
    style.defined.letterSpacing = 1;
  } else if (propNameBuf == "word-spacing") {
    if (propValueBuf == "normal") {
      style.wordSpacing = CssLength{};
    } else {
      style.wordSpacing = interpretLength(propValueBuf);
    }
    style.defined.wordSpacing = 1;
  } else if (propNameBuf == "margin-top") {
    style.marginTop = interpretLength(propValueBuf);
    style.defined.marginTop = 1;
  } else if (propNameBuf == "margin-bottom") {
    style.marginBottom = interpretLength(propValueBuf);
    style.defined.marginBottom = 1;
  } else if (propNameBuf == "margin-left") {
    style.marginLeft = interpretLength(propValueBuf);
    style.defined.marginLeft = 1;
  } else if (propNameBuf == "margin-right") {
    style.marginRight = interpretLength(propValueBuf);
    style.defined.marginRight = 1;
  } else if (propNameBuf == "margin") {
    const auto values = splitWhitespace(propValueBuf);
    if (!values.empty()) {
      style.marginTop = interpretLength(values[0]);
      style.marginRight = values.size() >= 2 ? interpretLength(values[1]) : style.marginTop;
      style.marginBottom = values.size() >= 3 ? interpretLength(values[2]) : style.marginTop;
      style.marginLeft = values.size() >= 4 ? interpretLength(values[3]) : style.marginRight;
      style.defined.marginTop = style.defined.marginRight = style.defined.marginBottom = style.defined.marginLeft = 1;
    }
  } else if (propNameBuf == "padding-top") {
    style.paddingTop = interpretLength(propValueBuf);
    style.defined.paddingTop = 1;
  } else if (propNameBuf == "padding-bottom") {
    style.paddingBottom = interpretLength(propValueBuf);
    style.defined.paddingBottom = 1;
  } else if (propNameBuf == "padding-left") {
    style.paddingLeft = interpretLength(propValueBuf);
    style.defined.paddingLeft = 1;
  } else if (propNameBuf == "padding-right") {
    style.paddingRight = interpretLength(propValueBuf);
    style.defined.paddingRight = 1;
  } else if (propNameBuf == "padding") {
    const auto values = splitWhitespace(propValueBuf);
    if (!values.empty()) {
      style.paddingTop = interpretLength(values[0]);
      style.paddingRight = values.size() >= 2 ? interpretLength(values[1]) : style.paddingTop;
      style.paddingBottom = values.size() >= 3 ? interpretLength(values[2]) : style.paddingTop;
      style.paddingLeft = values.size() >= 4 ? interpretLength(values[3]) : style.paddingRight;
      style.defined.paddingTop = style.defined.paddingRight = style.defined.paddingBottom = style.defined.paddingLeft =
          1;
    }
  } else if (propNameBuf == "display") {
    // Strip !important suffix if present
    std::string_view displayValue = propValueBuf;
    while (!displayValue.empty() && (displayValue.back() == ' ' || displayValue.back() == '\t')) {
      displayValue.remove_suffix(1);
    }
    constexpr std::string_view IMPORTANT = "!important";
    if (displayValue.size() >= IMPORTANT.size() &&
        displayValue.substr(displayValue.size() - IMPORTANT.size()) == IMPORTANT) {
      displayValue.remove_suffix(IMPORTANT.size());
      while (!displayValue.empty() && (displayValue.back() == ' ' || displayValue.back() == '\t')) {
        displayValue.remove_suffix(1);
      }
    }
    style.display = (displayValue == "none") ? CssDisplay::None : CssDisplay::Block;
    style.defined.display = 1;
  }
}

CssStyle CssParser::parseDeclarations(const std::string& declBlock) {
  CssStyle style;
  std::string propNameBuf;
  std::string propValueBuf;

  size_t start = 0;
  for (size_t i = 0; i <= declBlock.size(); ++i) {
    if (i == declBlock.size() || declBlock[i] == ';') {
      if (i > start) {
        const size_t len = i - start;
        std::string decl = declBlock.substr(start, len);
        if (!decl.empty()) {
          parseDeclarationIntoStyle(decl, style, propNameBuf, propValueBuf);
        }
      }
      start = i + 1;
    }
  }

  return style;
}

// Rule processing

void CssParser::processRuleBlockWithStyle(const std::string& selectorGroup, const CssStyle& style) {
  // Check if we've reached the rule limit before processing
  if (rulesBySelector_.size() >= MAX_RULES) {
    LOG_DBG("CSS", "Reached max rules limit (%zu), stopping CSS parsing", MAX_RULES);
    return;
  }

  // Handle comma-separated selectors
  const auto selectors = splitOnChar(selectorGroup, ',');

  for (const auto& sel : selectors) {
    // Validate selector length before processing
    if (sel.size() > MAX_SELECTOR_LENGTH) {
      LOG_DBG("CSS", "Selector too long (%zu > %zu), skipping", sel.size(), MAX_SELECTOR_LENGTH);
      continue;
    }

    // Normalize the selector
    std::string key = normalized(sel);
    if (key.empty()) continue;

    // Skip if this would exceed the rule limit
    if (rulesBySelector_.size() >= MAX_RULES) {
      LOG_DBG("CSS", "Reached max rules limit, stopping selector processing");
      return;
    }

    // Store or merge with existing
    auto it = rulesBySelector_.find(key);
    if (it != rulesBySelector_.end()) {
      it->second.applyOver(style);
    } else {
      rulesBySelector_[key] = style;
    }
  }
}

// Main parsing entry point

bool CssParser::loadFromStream(FsFile& source) {
  if (!source) {
    LOG_ERR("CSS", "Cannot read from invalid file");
    return false;
  }

  size_t totalRead = 0;

  // Use stack-allocated buffers for parsing to avoid heap reallocations
  StackBuffer selector;
  StackBuffer declBuffer;
  // Keep these as std::string since they're passed by reference to parseDeclarationIntoStyle
  std::string propNameBuf;
  std::string propValueBuf;

  bool inComment = false;
  bool maybeSlash = false;
  bool prevStar = false;

  bool inAtRule = false;
  int atDepth = 0;

  int bodyDepth = 0;
  bool skippingRule = false;
  CssStyle currentStyle;

  auto handleChar = [&](const char c) {
    if (inAtRule) {
      if (c == '{') {
        ++atDepth;
      } else if (c == '}') {
        if (atDepth > 0) --atDepth;
        if (atDepth == 0) inAtRule = false;
      } else if (c == ';' && atDepth == 0) {
        inAtRule = false;
      }
      return;
    }

    if (bodyDepth == 0) {
      if (selector.empty() && isCssWhitespace(c)) {
        return;
      }
      if (c == '@' && selector.empty()) {
        inAtRule = true;
        atDepth = 0;
        return;
      }
      if (c == '{') {
        bodyDepth = 1;
        currentStyle = CssStyle{};
        declBuffer.clear();
        if (selector.size() > MAX_SELECTOR_LENGTH * 4) {
          skippingRule = true;
        }
        return;
      }
      selector.push_back(c);
      return;
    }

    // bodyDepth > 0
    if (c == '{') {
      ++bodyDepth;
      return;
    }
    if (c == '}') {
      --bodyDepth;
      if (bodyDepth == 0) {
        if (!skippingRule && !declBuffer.empty()) {
          parseDeclarationIntoStyle(declBuffer.str(), currentStyle, propNameBuf, propValueBuf);
        }
        if (!skippingRule) {
          processRuleBlockWithStyle(selector.str(), currentStyle);
        }
        selector.clear();
        declBuffer.clear();
        skippingRule = false;
        return;
      }
      return;
    }
    if (bodyDepth > 1) {
      return;
    }
    if (!skippingRule) {
      if (c == ';') {
        if (!declBuffer.empty()) {
          parseDeclarationIntoStyle(declBuffer.str(), currentStyle, propNameBuf, propValueBuf);
          declBuffer.clear();
        }
      } else {
        declBuffer.push_back(c);
      }
    }
  };

  char buffer[READ_BUFFER_SIZE];
  while (source.available()) {
    int bytesRead = source.read(buffer, sizeof(buffer));
    if (bytesRead <= 0) break;

    totalRead += static_cast<size_t>(bytesRead);

    for (int i = 0; i < bytesRead; ++i) {
      const char c = buffer[i];

      if (inComment) {
        if (prevStar && c == '/') {
          inComment = false;
          prevStar = false;
          continue;
        }
        prevStar = c == '*';
        continue;
      }

      if (maybeSlash) {
        if (c == '*') {
          inComment = true;
          maybeSlash = false;
          prevStar = false;
          continue;
        }
        handleChar('/');
        maybeSlash = false;
        // fall through to process current char
      }

      if (c == '/') {
        maybeSlash = true;
        continue;
      }

      handleChar(c);
    }
  }

  if (maybeSlash) {
    handleChar('/');
  }

  LOG_DBG("CSS", "Parsed %zu rules from %zu bytes", rulesBySelector_.size(), totalRead);
  return true;
}

// Style resolution

CssStyle CssParser::resolveStyle(const std::string& tagName, const std::string& classAttr) const {
  static bool lowHeapWarningLogged = false;
  if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_CSS) {
    if (!lowHeapWarningLogged) {
      lowHeapWarningLogged = true;
      LOG_DBG("CSS", "Warning: low heap (%u bytes) below MIN_FREE_HEAP_FOR_CSS (%u), returning empty style",
              ESP.getFreeHeap(), static_cast<unsigned>(MIN_FREE_HEAP_FOR_CSS));
    }
    return CssStyle{};
  }

  // Helper: look up a selector and merge its style into `into`.
  // Transparently handles both RAM mode (rulesBySelector_ populated, e.g. during
  // build before cache is written) and disk-paged mode (offsetsBySelector_
  // populated, styles read from cache file on demand).
  const auto applySelectorInto = [this](const std::string& key, CssStyle& into) {
    if (!rulesBySelector_.empty()) {
      const auto it = rulesBySelector_.find(key);
      if (it != rulesBySelector_.end()) {
        into.applyOver(it->second);
      }
      return;
    }
    CssStyle cached;
    if (lruGet(key, cached)) {
      into.applyOver(cached);
      return;
    }
    const auto off = offsetsBySelector_.find(key);
    if (off == offsetsBySelector_.end()) {
      return;
    }
    CssStyle fromDisk;
    if (readStyleAtOffset(off->second, fromDisk)) {
      lruPut(key, fromDisk);
      into.applyOver(fromDisk);
    }
  };

  CssStyle result;
  const std::string tag = normalized(tagName);

  // 1. Apply element-level style (lowest priority)
  applySelectorInto(tag, result);

  // 2. Apply class styles (medium priority) then element.class (higher).
  if (!classAttr.empty()) {
    const auto classes = splitWhitespace(classAttr);
    for (const auto& cls : classes) {
      applySelectorInto("." + normalized(cls), result);
    }
    for (const auto& cls : classes) {
      applySelectorInto(tag + "." + normalized(cls), result);
    }
  }

  return result;
}

// Inline style parsing (static - doesn't need rule database)

CssStyle CssParser::parseInlineStyle(const std::string& styleValue) { return parseDeclarations(styleValue); }

// Cache serialization

// Cache format version - increment when format changes
constexpr uint8_t CSS_CACHE_VERSION = 4;
constexpr char rulesCache[] = "/css_rules.cache";

bool CssParser::hasCache() const { return Storage.exists((cachePath + rulesCache).c_str()); }

void CssParser::deleteCache() const {
  if (hasCache()) Storage.remove((cachePath + rulesCache).c_str());
}

bool CssParser::saveToCache() const {
  if (cachePath.empty()) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForWrite("CSS", cachePath + rulesCache, file)) {
    return false;
  }

  // Write version
  file.write(CSS_CACHE_VERSION);

  // Write rule count
  const auto ruleCount = static_cast<uint16_t>(rulesBySelector_.size());
  file.write(reinterpret_cast<const uint8_t*>(&ruleCount), sizeof(ruleCount));

  // Write each rule: selector string + CssStyle fields
  for (const auto& pair : rulesBySelector_) {
    // Write selector string (length-prefixed)
    const auto selectorLen = static_cast<uint16_t>(pair.first.size());
    file.write(reinterpret_cast<const uint8_t*>(&selectorLen), sizeof(selectorLen));
    file.write(reinterpret_cast<const uint8_t*>(pair.first.data()), selectorLen);

    // Write CssStyle fields (all are POD types)
    const CssStyle& style = pair.second;
    file.write(static_cast<uint8_t>(style.textAlign));
    file.write(static_cast<uint8_t>(style.fontStyle));
    file.write(static_cast<uint8_t>(style.fontWeight));
    file.write(static_cast<uint8_t>(style.textDecoration));

    // Write CssLength fields (value + unit)
    auto writeLength = [&file](const CssLength& len) {
      file.write(reinterpret_cast<const uint8_t*>(&len.value), sizeof(len.value));
      file.write(static_cast<uint8_t>(len.unit));
    };

    writeLength(style.textIndent);
    writeLength(style.marginTop);
    writeLength(style.marginBottom);
    writeLength(style.marginLeft);
    writeLength(style.marginRight);
    writeLength(style.paddingTop);
    writeLength(style.paddingBottom);
    writeLength(style.paddingLeft);
    writeLength(style.paddingRight);
    writeLength(style.lineHeight);
    writeLength(style.letterSpacing);
    writeLength(style.wordSpacing);
    file.write(static_cast<uint8_t>(style.display));

    // Write defined flags as uint32_t
    uint32_t definedBits = 0;
    if (style.defined.textAlign) definedBits |= 1 << 0;
    if (style.defined.fontStyle) definedBits |= 1 << 1;
    if (style.defined.fontWeight) definedBits |= 1 << 2;
    if (style.defined.textDecoration) definedBits |= 1 << 3;
    if (style.defined.textIndent) definedBits |= 1 << 4;
    if (style.defined.marginTop) definedBits |= 1 << 5;
    if (style.defined.marginBottom) definedBits |= 1 << 6;
    if (style.defined.marginLeft) definedBits |= 1 << 7;
    if (style.defined.marginRight) definedBits |= 1 << 8;
    if (style.defined.paddingTop) definedBits |= 1 << 9;
    if (style.defined.paddingBottom) definedBits |= 1 << 10;
    if (style.defined.paddingLeft) definedBits |= 1 << 11;
    if (style.defined.paddingRight) definedBits |= 1 << 12;
    if (style.defined.lineHeight) definedBits |= 1 << 13;
    if (style.defined.letterSpacing) definedBits |= 1 << 14;
    if (style.defined.wordSpacing) definedBits |= 1 << 15;
    if (style.defined.display) definedBits |= 1 << 16;
    file.write(reinterpret_cast<const uint8_t*>(&definedBits), sizeof(definedBits));
  }

  LOG_DBG("CSS", "Saved %u rules to cache", ruleCount);
  file.close();
  return true;
}

// Size in bytes of a serialized CssStyle body in v4 cache format. Used to
// skip past the style payload when building the disk-paged offset index.
//   4 enums (1 byte each)  = 4
//   12 CssLength (value 4 bytes + unit 1 byte) = 60
//   display (1 byte)       = 1
//   defined bits (4 bytes) = 4
static constexpr size_t CSS_STYLE_SERIALIZED_SIZE = 4 + 60 + 1 + 4;

// Read a full serialized CssStyle starting at `offset` in the open cacheFile_.
// Returns true on success. Deserialization mirrors the v4 format written by
// saveToCache.
bool CssParser::readStyleAtOffset(uint32_t offset, CssStyle& out) const {
  if (!cacheFileOpen_) {
    return false;
  }
  cacheFile_.seek(offset);

  uint8_t enumVal = 0;
  if (cacheFile_.read(&enumVal, 1) != 1) return false;
  out.textAlign = static_cast<CssTextAlign>(enumVal);
  if (cacheFile_.read(&enumVal, 1) != 1) return false;
  out.fontStyle = static_cast<CssFontStyle>(enumVal);
  if (cacheFile_.read(&enumVal, 1) != 1) return false;
  out.fontWeight = static_cast<CssFontWeight>(enumVal);
  if (cacheFile_.read(&enumVal, 1) != 1) return false;
  out.textDecoration = static_cast<CssTextDecoration>(enumVal);

  auto readLength = [this](CssLength& len) -> bool {
    if (cacheFile_.read(&len.value, sizeof(len.value)) != sizeof(len.value)) return false;
    uint8_t unitVal = 0;
    if (cacheFile_.read(&unitVal, 1) != 1) return false;
    len.unit = static_cast<CssUnit>(unitVal);
    return true;
  };
  if (!readLength(out.textIndent) || !readLength(out.marginTop) || !readLength(out.marginBottom) ||
      !readLength(out.marginLeft) || !readLength(out.marginRight) || !readLength(out.paddingTop) ||
      !readLength(out.paddingBottom) || !readLength(out.paddingLeft) || !readLength(out.paddingRight) ||
      !readLength(out.lineHeight) || !readLength(out.letterSpacing) || !readLength(out.wordSpacing)) {
    return false;
  }

  uint8_t displayVal = 0;
  if (cacheFile_.read(&displayVal, 1) != 1) return false;
  out.display = static_cast<CssDisplay>(displayVal);

  uint32_t definedBits = 0;
  if (cacheFile_.read(&definedBits, sizeof(definedBits)) != sizeof(definedBits)) return false;
  out.defined.textAlign = (definedBits & 1 << 0) != 0;
  out.defined.fontStyle = (definedBits & 1 << 1) != 0;
  out.defined.fontWeight = (definedBits & 1 << 2) != 0;
  out.defined.textDecoration = (definedBits & 1 << 3) != 0;
  out.defined.textIndent = (definedBits & 1 << 4) != 0;
  out.defined.marginTop = (definedBits & 1 << 5) != 0;
  out.defined.marginBottom = (definedBits & 1 << 6) != 0;
  out.defined.marginLeft = (definedBits & 1 << 7) != 0;
  out.defined.marginRight = (definedBits & 1 << 8) != 0;
  out.defined.paddingTop = (definedBits & 1 << 9) != 0;
  out.defined.paddingBottom = (definedBits & 1 << 10) != 0;
  out.defined.paddingLeft = (definedBits & 1 << 11) != 0;
  out.defined.paddingRight = (definedBits & 1 << 12) != 0;
  out.defined.lineHeight = (definedBits & 1 << 13) != 0;
  out.defined.letterSpacing = (definedBits & 1 << 14) != 0;
  out.defined.wordSpacing = (definedBits & 1 << 15) != 0;
  out.defined.display = (definedBits & 1 << 16) != 0;
  return true;
}

bool CssParser::lruGet(const std::string& key, CssStyle& out) const {
  for (auto it = lru_.begin(); it != lru_.end(); ++it) {
    if (it->first == key) {
      out = it->second;
      // Move to front (most recently used)
      if (it != lru_.begin()) {
        auto entry = std::move(*it);
        lru_.erase(it);
        lru_.push_front(std::move(entry));
      }
      return true;
    }
  }
  return false;
}

void CssParser::lruPut(const std::string& key, const CssStyle& value) const {
  for (auto it = lru_.begin(); it != lru_.end(); ++it) {
    if (it->first == key) {
      it->second = value;
      if (it != lru_.begin()) {
        auto entry = std::move(*it);
        lru_.erase(it);
        lru_.push_front(std::move(entry));
      }
      return;
    }
  }
  lru_.emplace_front(key, value);
  if (lru_.size() > LRU_CAP) {
    lru_.pop_back();
  }
}

void CssParser::clear() {
  rulesBySelector_.clear();
  offsetsBySelector_.clear();
  lru_.clear();
  if (cacheFileOpen_) {
    cacheFile_.close();
    cacheFileOpen_ = false;
  }
}

bool CssParser::loadFromCache() {
  if (cachePath.empty()) {
    return false;
  }

  // Drop any previous state (including a previously open cache file handle)
  // so we don't leak handles across repeated loads.
  clear();

  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, cacheFile_)) {
    return false;
  }
  cacheFileOpen_ = true;

  uint8_t version = 0;
  if (cacheFile_.read(&version, 1) != 1 || version != CSS_CACHE_VERSION) {
    LOG_DBG("CSS", "Cache version mismatch (got %u, expected %u)", version, CSS_CACHE_VERSION);
    cacheFile_.close();
    cacheFileOpen_ = false;
    return false;
  }

  uint16_t ruleCount = 0;
  if (cacheFile_.read(&ruleCount, sizeof(ruleCount)) != sizeof(ruleCount)) {
    cacheFile_.close();
    cacheFileOpen_ = false;
    return false;
  }

  // Pre-size the hash map to avoid repeated rehashes; a few rehashes at 470+
  // entries are enough to fragment the tight heap we're trying to conserve.
  offsetsBySelector_.reserve(ruleCount);

  // Walk each rule, record selector→offset-of-style mapping, skip past the
  // serialized style without deserializing it. Style bytes stay on SD and are
  // read on demand in resolveStyle.
  for (uint16_t i = 0; i < ruleCount; ++i) {
    uint16_t selectorLen = 0;
    if (cacheFile_.read(&selectorLen, sizeof(selectorLen)) != sizeof(selectorLen)) {
      clear();
      return false;
    }

    std::string selector;
    selector.resize(selectorLen);
    if (selectorLen > 0 && cacheFile_.read(&selector[0], selectorLen) != selectorLen) {
      clear();
      return false;
    }

    const uint32_t styleOffset = static_cast<uint32_t>(cacheFile_.position());
    // Skip style payload (fixed size in v4 format).
    cacheFile_.seek(styleOffset + CSS_STYLE_SERIALIZED_SIZE);

    offsetsBySelector_.emplace(std::move(selector), styleOffset);
  }

  LOG_DBG("CSS", "Indexed %u rules from cache (disk-paged)", ruleCount);
  return true;
}
