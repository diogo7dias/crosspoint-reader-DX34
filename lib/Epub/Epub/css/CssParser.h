#pragma once

#include <HalStorage.h>

#include <deque>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "CssStyle.h"

/**
 * Lightweight CSS parser for EPUB stylesheets
 *
 * Parses CSS files and extracts styling information relevant for e-ink display.
 * Uses a two-phase approach: first tokenizes the CSS content, then builds
 * a rule database that can be queried during HTML parsing.
 *
 * Supported selectors:
 *   - Element selectors: p, div, h1, etc.
 *   - Class selectors: .classname
 *   - Combined: element.classname
 *   - Grouped: selector1, selector2 { }
 *
 * Not supported (silently ignored):
 *   - Descendant/child selectors
 *   - Pseudo-classes and pseudo-elements
 *   - Media queries (content is skipped)
 *   - @import, @font-face, etc.
 */
class CssParser {
 public:
  explicit CssParser(std::string cachePath) : cachePath(std::move(cachePath)) {}
  ~CssParser() { clear(); }

  // Non-copyable
  CssParser(const CssParser&) = delete;
  CssParser& operator=(const CssParser&) = delete;

  /**
   * Load and parse CSS from a file stream.
   * Can be called multiple times to accumulate rules from multiple stylesheets.
   * @param source Open file handle to read from
   * @return true if parsing completed (even if no rules found)
   */
  bool loadFromStream(FsFile& source);

  /**
   * Look up the style for an HTML element, considering tag name and class attributes.
   * Applies CSS cascade: element style < class style < element.class style
   *
   * @param tagName The HTML element name (e.g., "p", "div")
   * @param classAttr The class attribute value (may contain multiple space-separated classes)
   * @return Combined style with all applicable rules merged
   */
  [[nodiscard]] CssStyle resolveStyle(const std::string& tagName, const std::string& classAttr) const;

  /**
   * Parse an inline style attribute string.
   * @param styleValue The value of a style="" attribute
   * @return Parsed style properties
   */
  [[nodiscard]] static CssStyle parseInlineStyle(const std::string& styleValue);

  /**
   * Check if any rules have been loaded (RAM map or disk-paged index)
   */
  [[nodiscard]] bool empty() const { return rulesBySelector_.empty() && hashedIndex_.empty(); }

  /**
   * Get count of loaded rule sets
   */
  [[nodiscard]] size_t ruleCount() const {
    return rulesBySelector_.empty() ? hashedIndex_.size() : rulesBySelector_.size();
  }

  /**
   * Clear all loaded rules and close any open cache file handle.
   * Safe to call multiple times.
   */
  void clear();

  /**
   * Check if CSS rules cache file exists
   */
  bool hasCache() const;

  /**
   * Delete CSS rules cache file
   */
  void deleteCache() const;

  /**
   * Save parsed CSS rules to a cache file.
   * @return true if cache was written successfully
   */
  bool saveToCache() const;

  /**
   * Load CSS rules from a cache file.
   * Clears any existing rules before loading.
   * @return true if cache was loaded successfully
   */
  bool loadFromCache();

 private:
  // Build-time: maps normalized selector -> style properties (fully in RAM).
  // Populated by parseCssFiles / loadFromStream; emptied after saveToCache +
  // loadFromCache. Not used during rendering.
  std::unordered_map<std::string, CssStyle> rulesBySelector_;

  // Render-time disk-paged index: a sorted vector of (selector hash, byte
  // offset of selector record in the CSS cache file). Replaces the prior
  // unordered_map<string, uint32_t> which cost ~70 bytes per entry (string
  // copy + node + bucket). At 470 rules that map was ~33 KB persistent
  // heap -- more than the largest contiguous free block on this device
  // after custom-font activation fragments memory, blocking section
  // rebuild for big books like Wolfe.
  //
  // New cost: 8 bytes per entry (~3.76 KB at 470 rules). Lookup is hash +
  // binary-search on the sorted vector; on hash hit the original selector
  // is read back from disk and strcmp'd to disambiguate collisions.
  // Collisions are rare for short ASCII selectors so the typical lookup
  // does one binary-search probe + one SD seek to read the verified
  // record + one read of the style payload (same I/O as before, just no
  // in-RAM string copies). The on-disk cache format is unchanged.
  struct HashedOffset {
    uint32_t hash;     // FNV-1a of normalized selector
    uint32_t record;   // Byte offset of selectorLen field in the cache file
  };
  std::vector<HashedOffset> hashedIndex_;

  // Kept open for the life of loaded cache so resolveStyle can seek+read.
  // Closed by clear().
  mutable FsFile cacheFile_;
  mutable bool cacheFileOpen_ = false;

  // Small LRU of recently-resolved styles keyed by selector. Avoids
  // re-reading the same rule from SD for every element that uses it.
  // Kept intentionally small (a few KB) so the savings vs the in-RAM map
  // are preserved on books with large stylesheets.
  static constexpr size_t LRU_CAP = 24;
  mutable std::deque<std::pair<std::string, CssStyle>> lru_;

  // Internal disk-paged helpers
  [[nodiscard]] bool readStyleAtOffset(uint32_t offset, CssStyle& out) const;
  // Resolve a selector to the file offset of its style payload, or return
  // false if no matching rule exists. Hashes the key, binary-searches
  // hashedIndex_, and verifies any hits by re-reading the selector record
  // off disk to handle hash collisions correctly.
  [[nodiscard]] bool findStyleOffsetForSelector(const std::string& key, uint32_t& styleOffsetOut) const;
  [[nodiscard]] bool lruGet(const std::string& key, CssStyle& out) const;
  void lruPut(const std::string& key, const CssStyle& value) const;

  std::string cachePath;

  // Internal parsing helpers
  void processRuleBlockWithStyle(const std::string& selectorGroup, const CssStyle& style);
  static CssStyle parseDeclarations(const std::string& declBlock);
  static void parseDeclarationIntoStyle(const std::string& decl, CssStyle& style, std::string& propNameBuf,
                                        std::string& propValueBuf);

  // Individual property value parsers
  static CssTextAlign interpretAlignment(const std::string& val);
  static CssFontStyle interpretFontStyle(const std::string& val);
  static CssFontWeight interpretFontWeight(const std::string& val);
  static CssTextDecoration interpretDecoration(const std::string& val);
  static CssLength interpretLength(const std::string& val);

  // String utilities
  static std::string normalized(const std::string& s);
  static void normalizedInto(const std::string& s, std::string& out);
  static std::vector<std::string> splitOnChar(const std::string& s, char delimiter);
  static std::vector<std::string> splitWhitespace(const std::string& s);
};
