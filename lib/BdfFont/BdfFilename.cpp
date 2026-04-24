#include "BdfFilename.h"

#include <cctype>
#include <cstring>

namespace crosspoint {
namespace bdf {

namespace {

bool isNameChar(const char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

// Case-insensitive extension match against ".bdf". Returns end-of-name index
// (length minus 4) on match, -1 otherwise.
int stripBdfExtension(const char* basename) {
  const size_t len = std::strlen(basename);
  if (len < 5) return -1;  // need at least "x.bdf"
  const size_t extStart = len - 4;
  if (basename[extStart] != '.') return -1;
  const char e1 = static_cast<char>(std::tolower(static_cast<unsigned char>(basename[extStart + 1])));
  const char e2 = static_cast<char>(std::tolower(static_cast<unsigned char>(basename[extStart + 2])));
  const char e3 = static_cast<char>(std::tolower(static_cast<unsigned char>(basename[extStart + 3])));
  if (e1 != 'b' || e2 != 'd' || e3 != 'f') return -1;
  return static_cast<int>(extStart);
}

// Case-insensitive suffix test on [base, base+len). Returns true when the
// trailing characters equal `suffix` (preceded by '-') — e.g. suffixMatches
// ("unifont-bold", 12, "bold") returns true, suffixMatches("foobold", 7,
// "bold") returns false because there is no '-' before "bold".
bool suffixMatches(const char* base, int len, const char* suffix) {
  const int sufLen = static_cast<int>(std::strlen(suffix));
  // Need at least one name char + '-' + suffix chars.
  if (len < sufLen + 2) return false;
  if (base[len - sufLen - 1] != '-') return false;
  for (int i = 0; i < sufLen; ++i) {
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(base[len - sufLen + i])));
    if (c != suffix[i]) return false;
  }
  return true;
}

// Strips a trailing variant tag from a name span. Accepts plural-s and
// "-regular" aliases so the tool user's naming (-italics, -Regular, etc)
// still parses. On match, updates `nameLen` to the new (shorter) length and
// sets `variant` to the corresponding tag. On miss, leaves both untouched.
// Check bolditalic(s) BEFORE bold/italic so "unifont-bolditalic" doesn't get
// greedy-matched against -italic (which would leave "unifont-bold").
void stripVariantSuffix(const char* base, int& nameLen, BdfVariant& variant) {
  auto tryStrip = [&](const char* suffix, BdfVariant v) -> bool {
    if (!suffixMatches(base, nameLen, suffix)) return false;
    variant = v;
    nameLen -= static_cast<int>(std::strlen(suffix)) + 1;  // +1 for '-'
    return true;
  };
  if (tryStrip("bolditalics", BdfVariant::BoldItalic)) return;
  if (tryStrip("bolditalic", BdfVariant::BoldItalic)) return;
  if (tryStrip("bold", BdfVariant::Bold)) return;
  if (tryStrip("italics", BdfVariant::Italic)) return;
  if (tryStrip("italic", BdfVariant::Italic)) return;
  if (tryStrip("regular", BdfVariant::Regular)) return;
}

}  // namespace

std::optional<BdfFilenameInfo> parseBdfFilename(const char* basename) {
  if (!basename) return std::nullopt;

  const int extPos = stripBdfExtension(basename);
  if (extPos <= 0) return std::nullopt;

  // Find the last underscore before the extension.
  int underscorePos = -1;
  for (int i = extPos - 1; i >= 0; --i) {
    if (basename[i] == '_') {
      underscorePos = i;
      break;
    }
  }
  if (underscorePos <= 0) return std::nullopt;           // no underscore, or empty name
  if (underscorePos == extPos - 1) return std::nullopt;  // "_.bdf" or "foo_.bdf"

  // Validate name chars in [0, underscorePos).
  for (int i = 0; i < underscorePos; ++i) {
    if (!isNameChar(basename[i])) return std::nullopt;
  }

  // Parse size digits in [underscorePos+1, extPos).
  uint32_t size = 0;
  for (int i = underscorePos + 1; i < extPos; ++i) {
    const char c = basename[i];
    if (c < '0' || c > '9') return std::nullopt;
    size = size * 10 + static_cast<uint32_t>(c - '0');
    if (size > 255) return std::nullopt;
  }
  if (size < 4) return std::nullopt;

  // Peel off optional variant tag from the tail of the family-name span.
  int nameLen = underscorePos;
  BdfVariant variant = BdfVariant::Regular;
  stripVariantSuffix(basename, nameLen, variant);
  if (nameLen <= 0) return std::nullopt;  // "-bold_16.bdf" — no family name left

  BdfFilenameInfo info;
  info.name.assign(basename, static_cast<size_t>(nameLen));
  info.sizePt = static_cast<uint16_t>(size);
  info.variant = variant;
  return info;
}

}  // namespace bdf
}  // namespace crosspoint
