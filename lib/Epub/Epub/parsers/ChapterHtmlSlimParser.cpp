#include "ChapterHtmlSlimParser.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <HeapGuard.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <expat.h>

#include "../../Epub.h"
#include "../Page.h"
#include "../converters/ImageDecoderFactory.h"
#include "../converters/ImageToFramebufferDecoder.h"
#include "../htmlEntities.h"
#include "../hyphenation/Hyphenator.h"

const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr int NUM_HEADER_TAGS = sizeof(HEADER_TAGS) / sizeof(HEADER_TAGS[0]);

// Minimum file size (in bytes) to show indexing progress - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_PROGRESS = 10 * 1024;  // 10KB
constexpr int PROGRESS_STEP_PERCENT = 5;
constexpr int MIN_DENSE_PAGE_LINES = 6;
constexpr int DENSE_PAGE_THRESHOLD_PERCENT = 80;

const char* BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote"};
constexpr int NUM_BLOCK_TAGS = sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]);

const char* BOLD_TAGS[] = {"b", "strong"};
constexpr int NUM_BOLD_TAGS = sizeof(BOLD_TAGS) / sizeof(BOLD_TAGS[0]);

const char* ITALIC_TAGS[] = {"i", "em"};
constexpr int NUM_ITALIC_TAGS = sizeof(ITALIC_TAGS) / sizeof(ITALIC_TAGS[0]);

const char* UNDERLINE_TAGS[] = {"u", "ins"};
constexpr int NUM_UNDERLINE_TAGS = sizeof(UNDERLINE_TAGS) / sizeof(UNDERLINE_TAGS[0]);

const char* IMAGE_TAGS[] = {"img"};
constexpr int NUM_IMAGE_TAGS = sizeof(IMAGE_TAGS) / sizeof(IMAGE_TAGS[0]);

const char* SKIP_TAGS[] = {"head"};
constexpr int NUM_SKIP_TAGS = sizeof(SKIP_TAGS) / sizeof(SKIP_TAGS[0]);

bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

// given the start and end of a tag, check to see if it matches a known tag
bool matches(const char* tag_name, const char* possible_tags[], const int possible_tag_count) {
  for (int i = 0; i < possible_tag_count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

const char* getAttribute(const XML_Char** atts, const char* attrName) {
  if (!atts) return nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], attrName) == 0) return atts[i + 1];
  }
  return nullptr;
}

bool isInternalEpubLink(const char* href) {
  if (!href || href[0] == '\0') return false;
  if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) return false;
  if (strncmp(href, "mailto:", 7) == 0) return false;
  if (strncmp(href, "ftp://", 6) == 0) return false;
  if (strncmp(href, "tel:", 4) == 0) return false;
  if (strncmp(href, "javascript:", 11) == 0) return false;
  return true;
}

bool isHeaderOrBlock(const char* name) {
  return matches(name, HEADER_TAGS, NUM_HEADER_TAGS) || matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS);
}

// Update effective bold/italic/underline based on block style and inline style stack
// flush the contents of partWordBuffer to currentTextBlock
void ChapterHtmlSlimParser::flushPartWordBuffer() {
  // Resolve the effective style for this word from the three former systems
  // (depth flags + inline stack + block CSS base), now behind StyleResolver.
  const EpdFontFamily::Style fontStyle = styleResolver_.effectiveStyle(depth);

  // flush the buffer
  partWordBuffer[partWordBufferIndex] = '\0';
  if (!currentTextBlock->addWord(partWordBuffer, static_cast<size_t>(partWordBufferIndex), fontStyle, false,
                                 nextWordContinues)) {
    // Heap-probe inside addWord bailed before the next vector growth would
    // have crashed. Promote to parser-level failure so createSectionFile
    // returns false and the recovery screen path takes over.
    LOG_DIAG("EHP", "addWord OOM (flush) free=%u largest=%u", (unsigned)ESP.getFreeHeap(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    parseFailed = true;
  }
  partWordBufferIndex = 0;
  nextWordContinues = false;
}

void ChapterHtmlSlimParser::completeCurrentPage() {
  if (!currentPage) {
    return;
  }

  const int lineHeight = static_cast<int>(renderer.getLineHeight(fontId) * lineCompression);
  if (lineHeight > 0 && viewportHeight > 0) {
    const int maxPossibleLines = viewportHeight / lineHeight;
    const int minDenseLines = std::max(MIN_DENSE_PAGE_LINES, (maxPossibleLines * DENSE_PAGE_THRESHOLD_PERCENT) / 100);
    currentPage->applyDensePageVerticalFit(lineHeight, viewportHeight, minDenseLines, lineHeight / 2);
  }

  completePageFn(std::move(currentPage));
  completedPageCount++;
}

void ChapterHtmlSlimParser::bindPendingAnchorsToCurrentPage() {
  if (pendingAnchors.empty()) {
    return;
  }
  if (parseFailed) return;

  if (!currentPage) {
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_DIAG("EHP", "OOM new Page (bindPendingAnchors) free=%u largest=%u", (unsigned)ESP.getFreeHeap(),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      parseFailed = true;
      return;
    }
    currentPageNextY = 0;
  }

  for (const auto& anchor : pendingAnchors) {
    if (!anchor.empty()) {
      anchorPageFn(anchor, completedPageCount);
    }
  }
  pendingAnchors.clear();
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const BlockStyle& blockStyle) {
  nextWordContinues = false;  // New block = new paragraph, no continuation
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      // Merge with existing block style to accumulate CSS styling from parent block elements.
      // This handles cases like <div style="margin-bottom:2em"><h1>text</h1></div> where the
      // div's margin should be preserved, even though it has no direct text content.
      currentTextBlock->setBlockStyle(currentTextBlock->blockStyle().getCombinedBlockStyle(blockStyle));
      return;
    }

    makePages();
  }
  // RFC #164 step 7: the degradation plan's hyphenate lever gates the parser's
  // own hyphenation setting — under heap pressure (NoHyphen+) we skip the
  // hyphenation/oversized-split passes whose mid-vector inserts churn the heap.
  currentTextBlock.reset(new (std::nothrow) crosspoint::layout::LayoutEngine(
      renderer, fontId, extraParagraphSpacingLevel != 0, hyphenationEnabled && degradePlan_.hyphenate, blockStyle,
      wordSpacingPercent, firstLineIndentMode, usePublisherStyles, sectionArena_, degradePlan_));
  if (!currentTextBlock) {
    LOG_DIAG("EHP", "OOM new LayoutEngine free=%u largest=%u min=%u", (unsigned)ESP.getFreeHeap(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap());
    parseFailed = true;
    return;
  }
  footnotePlacer_.onNewBlock();
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  // Extract class and style attributes for CSS processing
  std::string classAttr;
  std::string styleAttr;
  std::vector<std::string> anchors;
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "class") == 0) {
        classAttr = atts[i + 1];
      } else if (strcmp(atts[i], "style") == 0) {
        styleAttr = atts[i + 1];
      } else if ((strcmp(atts[i], "id") == 0 || strcmp(atts[i], "xml:id") == 0 ||
                  (strcmp(atts[i], "name") == 0 && strcmp(name, "a") == 0)) &&
                 atts[i + 1] != nullptr && atts[i + 1][0] != '\0') {
        anchors.emplace_back(atts[i + 1]);
      }
    }
  }

  if (!anchors.empty()) {
    self->pendingAnchors.insert(self->pendingAnchors.end(), anchors.begin(), anchors.end());
  }

  // Compute CSS style for this element early so display:none can short-circuit
  // before tag-specific branches emit any content or metadata.
  CssStyle cssStyle;
  if (self->cssParser) {
    cssStyle = self->cssParser->resolveStyle(name, classAttr);
    if (!styleAttr.empty()) {
      CssStyle inlineStyle = CssParser::parseInlineStyle(styleAttr);
      cssStyle.applyOver(inlineStyle);
    }
  }

  // Skip elements with display:none before all fast paths (tables, links, etc.).
  if (cssStyle.hasDisplay() && cssStyle.display == CssDisplay::None) {
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  auto centeredBlockStyle = BlockStyle();
  centeredBlockStyle.textAlignDefined = true;
  centeredBlockStyle.alignment = CssTextAlign::Center;

  // Special handling for tables - show placeholder text instead of dropping silently
  if (strcmp(name, "table") == 0) {
    // Add placeholder text
    self->startNewTextBlock(centeredBlockStyle);

    self->styleResolver_.setItalicFrom(self->depth);
    // Advance depth before processing character data (like you would for an element with text)
    self->depth += 1;
    self->characterData(userData, "[Table omitted]", strlen("[Table omitted]"));

    // Skip table contents (skip until parent as we pre-advanced depth above)
    self->skipUntilDepth = self->depth - 1;
    return;
  }

  if (matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS)) {
    std::string src;
    std::string alt;
    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "src") == 0) {
          src = atts[i + 1];
        } else if (strcmp(atts[i], "alt") == 0) {
          alt = atts[i + 1];
        }
      }

      // RFC #164: degradePlan_.images is the SkipImages lever (Full keeps it
      // true — unchanged). Under heap pressure the section is laid out without
      // image blocks rather than OOM-restarting.
      if (!src.empty() && self->degradePlan_.images) {
        LOG_DBG("EHP", "Found image: src=%s", src.c_str());

        {
          // Resolve the image path relative to the HTML file
          std::string resolvedPath = FsHelpers::normalisePath(self->contentBase + src);

          // Create a unique filename for the cached image
          std::string ext;
          size_t extPos = resolvedPath.rfind('.');
          if (extPos != std::string::npos) {
            ext = resolvedPath.substr(extPos);
          }
          std::string cachedImagePath = self->imageBasePath + std::to_string(self->imageCounter++) + ext;

          // Extract image to cache file
          FsFile cachedImageFile;
          bool extractSuccess = false;
          if (Storage.openFileForWrite("EHP", cachedImagePath, cachedImageFile)) {
            extractSuccess = self->epub->readItemContentsToStream(resolvedPath, cachedImageFile, 4096);
            cachedImageFile.flush();
            cachedImageFile.close();
            delay(50);  // Give SD card time to sync
          }

          if (extractSuccess) {
            // Get image dimensions
            ImageDimensions dims = {0, 0};
            ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(cachedImagePath);
            if (decoder && decoder->getDimensions(cachedImagePath, dims)) {
              LOG_DBG("EHP", "Image dimensions: %dx%d", dims.width, dims.height);

              // Scale to fit viewport while maintaining aspect ratio
              int maxWidth = self->viewportWidth;
              int maxHeight = self->viewportHeight;
              float scaleX = (dims.width > maxWidth) ? (float)maxWidth / dims.width : 1.0f;
              float scaleY = (dims.height > maxHeight) ? (float)maxHeight / dims.height : 1.0f;
              float scale = (scaleX < scaleY) ? scaleX : scaleY;
              if (scale > 1.0f) scale = 1.0f;

              int displayWidth = (int)(dims.width * scale);
              int displayHeight = (int)(dims.height * scale);

              LOG_DBG("EHP", "Display size: %dx%d (scale %.2f)", displayWidth, displayHeight, scale);

              // Create page for image - only break if image won't fit remaining space.
              // The pre-existing `new Page()` calls below were bare and would
              // throw bad_alloc on a fragmented heap; with exceptions disabled
              // in the firmware build that aborts. Switch to nothrow and treat
              // a null result as a parse-aborting OOM (poll the flag from the
              // outer parse loop to bail with the standard cleanup path).
              if (self->currentPage && !self->currentPage->elements.empty() &&
                  (self->currentPageNextY + displayHeight > self->viewportHeight)) {
                self->completeCurrentPage();
                self->currentPage.reset(new (std::nothrow) Page());
                if (!self->currentPage) {
                  LOG_DIAG("EHP", "OOM new Page (image overflow) free=%u largest=%u min=%u",
                           (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                           (unsigned)ESP.getMinFreeHeap());
                  self->parseFailed = true;
                  return;
                }
                self->currentPageNextY = 0;
              } else if (!self->currentPage) {
                self->currentPage.reset(new (std::nothrow) Page());
                if (!self->currentPage) {
                  LOG_DIAG("EHP", "OOM new Page (image initial) free=%u largest=%u min=%u", (unsigned)ESP.getFreeHeap(),
                           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap());
                  self->parseFailed = true;
                  return;
                }
                self->currentPageNextY = 0;
              }

              self->bindPendingAnchorsToCurrentPage();

              // Create ImageBlock and add to page. make_shared without nothrow
              // throws bad_alloc on heap exhaustion → ESP32 panic. Use the
              // explicit shared_ptr(new (nothrow) ...) form so we can detect
              // and bail.
              auto* rawImageBlock = new (std::nothrow) ImageBlock(cachedImagePath, displayWidth, displayHeight);
              if (!rawImageBlock) {
                LOG_DIAG("EHP", "OOM new ImageBlock free=%u largest=%u", (unsigned)ESP.getFreeHeap(),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
                self->parseFailed = true;
                return;
              }
              std::shared_ptr<ImageBlock> imageBlock(rawImageBlock);
              int xPos = (self->viewportWidth - displayWidth) / 2;
              auto* rawPageImage = new (std::nothrow) PageImage(imageBlock, xPos, self->currentPageNextY);
              if (!rawPageImage) {
                LOG_DIAG("EHP", "OOM new PageImage free=%u largest=%u", (unsigned)ESP.getFreeHeap(),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
                self->parseFailed = true;
                return;
              }
              std::shared_ptr<PageImage> pageImage(rawPageImage);
              self->currentPage->elements.push_back(pageImage);
              self->currentPageNextY += displayHeight;

              self->depth += 1;
              return;
            } else {
              LOG_ERR("EHP", "Failed to get image dimensions");
              Storage.remove(cachedImagePath.c_str());
            }
          } else {
            LOG_ERR("EHP", "Failed to extract image");
          }
        }
      }

      // Fallback to alt text if image processing fails
      if (!alt.empty()) {
        alt = "[Image: " + alt + "]";
        self->startNewTextBlock(centeredBlockStyle);
        self->styleResolver_.setItalicFrom(self->depth);
        self->depth += 1;
        self->characterData(userData, alt.c_str(), alt.length());
        // Skip any child content (skip until parent as we pre-advanced depth above)
        self->skipUntilDepth = self->depth - 1;
        return;
      }

      // No alt text, skip
      self->skipUntilDepth = self->depth;
      self->depth += 1;
      return;
    }
  }

  if (matches(name, SKIP_TAGS, NUM_SKIP_TAGS)) {
    // start skip
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Skip blocks with role="doc-pagebreak" and epub:type="pagebreak"
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "role") == 0 && strcmp(atts[i + 1], "doc-pagebreak") == 0 ||
          strcmp(atts[i], "epub:type") == 0 && strcmp(atts[i + 1], "pagebreak") == 0) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }
    }
  }

  // Detect internal <a href="..."> links (footnotes, cross-references)
  if (strcmp(name, "a") == 0) {
    const char* href = getAttribute(atts, "href");

    bool isInternalLink = isInternalEpubLink(href);

    // Special case: javascript:void(0) links with data attributes
    if (href && strncmp(href, "javascript:", 11) == 0) {
      isInternalLink = false;
    }

    if (isInternalLink) {
      // Flush buffer before style change
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      self->insideFootnoteLink = true;
      self->footnoteLinkDepth = self->depth;
      strncpy(self->currentFootnote.href, href, sizeof(self->currentFootnote.href) - 1);
      self->currentFootnote.href[sizeof(self->currentFootnote.href) - 1] = '\0';
      self->currentFootnote.number[0] = '\0';
      self->currentFootnoteLinkTextLen = 0;

      // Apply underline style to visually indicate the link
      self->styleResolver_.setUnderlineFrom(self->depth);
      StyleResolver::InlineStyle entry;
      entry.hasUnderline = true;
      entry.underline = true;
      self->styleResolver_.pushInline(self->depth, entry);

      // Skip CSS resolution — we already handled styling for this <a> tag
      self->depth += 1;
      return;
    }
  }

  // If publisher styles are disabled, clear the CSS style so only display:none
  // (already applied above) takes effect — layout properties are user-controlled.
  if (!self->usePublisherStyles) {
    CssStyle displayOnly;
    if (cssStyle.hasDisplay()) {
      displayOnly.display = cssStyle.display;
      displayOnly.defined.display = 1;
    }
    cssStyle = displayOnly;
  } else {
    // Hybrid mode: honour publisher layout styles except line-height, which
    // the user controls via the line-spacing setting.
    cssStyle.defined.lineHeight = 0;
  }

  const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
  const auto userAlignmentBlockStyle = BlockStyle::fromCssStyle(
      cssStyle, emSize, static_cast<CssTextAlign>(self->paragraphAlignment), self->viewportWidth);

  if (matches(name, HEADER_TAGS, NUM_HEADER_TAGS)) {
    self->styleResolver_.setCssBase(cssStyle);
    auto headerBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Center, self->viewportWidth);
    headerBlockStyle.textAlignDefined = true;
    if (self->usePublisherStyles && cssStyle.hasTextAlign()) {
      headerBlockStyle.alignment = cssStyle.textAlign;
    }
    self->startNewTextBlock(headerBlockStyle);
    self->styleResolver_.setBoldFrom(self->depth);
  } else if (matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS)) {
    if (strcmp(name, "br") == 0) {
      if (self->partWordBufferIndex > 0) {
        // flush word preceding <br/> to currentTextBlock before calling startNewTextBlock
        self->flushPartWordBuffer();
      }
      self->startNewTextBlock(self->currentTextBlock->blockStyle());
    } else {
      self->styleResolver_.setCssBase(cssStyle);
      self->startNewTextBlock(userAlignmentBlockStyle);

      if (strcmp(name, "li") == 0) {
        // U+2022 bullet, 3 UTF-8 bytes.
        if (!self->currentTextBlock->addWord("\xe2\x80\xa2", 3, EpdFontFamily::REGULAR)) {
          LOG_DIAG("EHP", "addWord OOM (li bullet) free=%u largest=%u", (unsigned)ESP.getFreeHeap(),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
          self->parseFailed = true;
        }
      }
    }
  } else if (matches(name, UNDERLINE_TAGS, NUM_UNDERLINE_TAGS)) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->styleResolver_.setUnderlineFrom(self->depth);
    // Push inline style entry for underline tag
    StyleResolver::InlineStyle entry;
    entry.hasUnderline = true;
    entry.underline = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    self->styleResolver_.pushInline(self->depth, entry);
  } else if (matches(name, BOLD_TAGS, NUM_BOLD_TAGS)) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->styleResolver_.setBoldFrom(self->depth);
    // Push inline style entry for bold tag
    StyleResolver::InlineStyle entry;
    entry.hasBold = true;
    entry.bold = true;
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    if (cssStyle.hasTextDecoration()) {
      entry.hasUnderline = true;
      entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
    }
    self->styleResolver_.pushInline(self->depth, entry);
  } else if (matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS)) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->styleResolver_.setItalicFrom(self->depth);
    // Push inline style entry for italic tag
    StyleResolver::InlineStyle entry;
    entry.hasItalic = true;
    entry.italic = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    if (cssStyle.hasTextDecoration()) {
      entry.hasUnderline = true;
      entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
    }
    self->styleResolver_.pushInline(self->depth, entry);
  } else if (strcmp(name, "span") == 0 || !isHeaderOrBlock(name)) {
    // Handle span and other inline elements for CSS styling
    if (cssStyle.hasFontWeight() || cssStyle.hasFontStyle() || cssStyle.hasTextDecoration()) {
      // Flush buffer before style change so preceding text gets current style
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      StyleResolver::InlineStyle entry;
      if (cssStyle.hasFontWeight()) {
        entry.hasBold = true;
        entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
      }
      if (cssStyle.hasFontStyle()) {
        entry.hasItalic = true;
        entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
      }
      if (cssStyle.hasTextDecoration()) {
        entry.hasUnderline = true;
        entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
      }
      self->styleResolver_.pushInline(self->depth, entry);
    }
  }

  // Unprocessed tag, just increasing depth and continue forward
  self->depth += 1;
}

void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    return;
  }

  // Collect footnote link display text (for the number label)
  // Skip whitespace and brackets to normalize noterefs like "[1]" → "1"
  if (self->insideFootnoteLink) {
    int start = 0;
    int end = len - 1;

    // Example input and output texts:
    // "     [  12  ]   " => "12"
    // "   turn to 256  " => "turn to 256"

    // Ignore leading whitespaces and left square brackets
    while (start < len && (isWhitespace(s[start]) || (s[start] == '['))) {
      ++start;
    }

    // Ignore trailing whitespaces and right square brackets
    while (end >= start && (isWhitespace(s[end]) || (s[end] == ']'))) {
      --end;
    }

    // Extract footnote link text
    for (int i = start; (self->currentFootnoteLinkTextLen < sizeof(self->currentFootnote.number) - 1) && (i <= end);
         ++i) {
      self->currentFootnote.number[self->currentFootnoteLinkTextLen++] = s[i];
    }
    self->currentFootnote.number[self->currentFootnoteLinkTextLen] = '\0';
  }

  for (int i = 0; i < len; i++) {
    if (isWhitespace(s[i])) {
      // Currently looking at whitespace, if there's anything in the partWordBuffer, flush it
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      // Whitespace is a real word boundary — reset continuation state
      self->nextWordContinues = false;
      // Skip the whitespace char
      continue;
    }

    // Detect U+00A0 (non-breaking space): UTF-8 encoding is 0xC2 0xA0
    // Render a visible space without allowing a line break around it.
    if (static_cast<uint8_t>(s[i]) == 0xC2 && i + 1 < len && static_cast<uint8_t>(s[i + 1]) == 0xA0) {
      // Flush any pending text so style is applied correctly.
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      // Add a standalone space that attaches to the previous word.
      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->nextWordContinues = true;  // Attach space to previous word (no break).
      self->flushPartWordBuffer();

      // Ensure the next real word attaches to this space (no break).
      self->nextWordContinues = true;

      i++;  // Skip the second byte (0xA0)
      continue;
    }

    // Skip Zero Width No-Break Space / BOM (U+FEFF) = 0xEF 0xBB 0xBF
    const XML_Char FEFF_BYTE_1 = static_cast<XML_Char>(0xEF);
    const XML_Char FEFF_BYTE_2 = static_cast<XML_Char>(0xBB);
    const XML_Char FEFF_BYTE_3 = static_cast<XML_Char>(0xBF);

    if (s[i] == FEFF_BYTE_1) {
      // Check if the next two bytes complete the 3-byte sequence
      if ((i + 2 < len) && (s[i + 1] == FEFF_BYTE_2) && (s[i + 2] == FEFF_BYTE_3)) {
        // Sequence 0xEF 0xBB 0xBF found!
        i += 2;    // Skip the next two bytes
        continue;  // Move to the next iteration
      }
    }

    // If we're about to run out of space, then cut the word off and start a new one
    if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
      self->flushPartWordBuffer();
    }

    self->partWordBuffer[self->partWordBufferIndex++] = s[i];
  }

  // If we have > 750 words buffered up, perform the layout and consume out all but the last line
  // There should be enough here to build out 1-2 full pages and doing this will free up a lot of
  // memory.
  // Spotted when reading Intermezzo, there are some really long text blocks in there.
  if (self->currentTextBlock->wordCount() > 750) {
    LOG_DBG("EHP", "Text block too long, splitting into multiple pages");
    const int horizontalInset = self->currentTextBlock->blockStyle().totalHorizontalInset();
    const uint16_t effectiveWidth = (horizontalInset < self->viewportWidth)
                                        ? static_cast<uint16_t>(self->viewportWidth - horizontalInset)
                                        : self->viewportWidth;
    const crosspoint::layout::LayoutStatus status = self->currentTextBlock->flush(
        effectiveWidth, [self](const std::shared_ptr<TextBlock>& textBlock) { self->addLineToPage(textBlock); }, false);
    if (status != crosspoint::layout::LayoutStatus::Ok) {
      self->parseFailed = true;
      return;
    }
  }
}

void XMLCALL ChapterHtmlSlimParser::defaultHandlerExpand(void* userData, const XML_Char* s, const int len) {
  // Check if this looks like an entity reference (&...;)
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    const char* utf8Value = lookupHtmlEntity(s, static_cast<size_t>(len));
    if (utf8Value != nullptr) {
      // Known entity: expand to its UTF-8 value
      characterData(userData, utf8Value, strlen(utf8Value));
      return;
    }
    // Unknown entity: preserve original &...; sequence
    characterData(userData, s, len);
    return;
  }
  // Not an entity we recognize - skip it
}

void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Check if any style state will change after we decrement depth
  // If so, we MUST flush the partWordBuffer with the CURRENT style first
  // Note: depth hasn't been decremented yet, so we check against (depth - 1)
  const bool styleWillChange = self->styleResolver_.wouldChangeAt(self->depth - 1);
  const bool headerOrBlockTag = isHeaderOrBlock(name);

  // Flush buffer with current style BEFORE any style changes
  if (self->partWordBufferIndex > 0) {
    // Flush if style will change OR if we're closing a block/structural element
    const bool isInlineTag = !headerOrBlockTag && strcmp(name, "table") != 0 &&
                             !matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS) && self->depth != 1;
    const bool shouldFlush = styleWillChange || headerOrBlockTag || matches(name, BOLD_TAGS, NUM_BOLD_TAGS) ||
                             matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS) ||
                             matches(name, UNDERLINE_TAGS, NUM_UNDERLINE_TAGS) || strcmp(name, "table") == 0 ||
                             matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS) || self->depth == 1;

    if (shouldFlush) {
      self->flushPartWordBuffer();
      // If closing an inline element, the next word fragment continues the same visual word
      if (isInlineTag) {
        self->nextWordContinues = true;
      }
    }
  }

  self->depth -= 1;

  // Closing a footnote link — create entry from collected text and href
  if (self->insideFootnoteLink && self->depth == self->footnoteLinkDepth) {
    if (self->currentFootnote.number[0] != '\0' && self->currentFootnote.href[0] != '\0') {
      FootnoteEntry entry;
      strncpy(entry.number, self->currentFootnote.number, sizeof(entry.number) - 1);
      entry.number[sizeof(entry.number) - 1] = '\0';
      strncpy(entry.href, self->currentFootnote.href, sizeof(entry.href) - 1);
      entry.href[sizeof(entry.href) - 1] = '\0';
      int wordIndex = self->footnotePlacer_.extractedWordCount() +
                      (self->currentTextBlock ? static_cast<int>(self->currentTextBlock->wordCount()) : 0);
      self->footnotePlacer_.registerFootnote(wordIndex, entry);
    }
    self->insideFootnoteLink = false;
  }

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
  }

  // Leaving bold/italic/underline tags: clear any depth flag set at this depth.
  self->styleResolver_.clearDepthFlagsAt(self->depth);

  // Pop from inline style stack if we pushed an entry at this depth
  // This handles all inline elements: b, i, u, span, etc.
  self->styleResolver_.popInlineAtDepth(self->depth);

  // Clear block style when leaving header or block elements
  if (headerOrBlockTag) {
    self->styleResolver_.clearCssBase();
  }
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
  // RFC #164 steps 3+6: pick the layout arena for this section. If the reader
  // activity handed us its section-lifetime arena (the repurposed 24 KB anchor),
  // use that — it was reserved at onEnter while the heap was fresh, so it is
  // available even now that the heap is fragmented (the whole point). Reset it
  // first so this section starts from an empty block (the prior section's LIFO
  // rewinds should already leave it empty; this is belt-and-suspenders). Else
  // fall back to the legacy self-create: a per-section block, but only when the
  // heap has comfortable headroom (>= 3x), so grabbing it can never tip a
  // borderline section into a words[] OOM it would otherwise have survived. On a
  // tight heap with no external arena, sectionArena_ stays empty (ok()==false)
  // and layout falls back to std::vector — byte-identical to today. Set before
  // the first startNewTextBlock so every paragraph's engine sees a stable arena.
  if (externalArena_ != nullptr && externalArena_->ok()) {
    externalArena_->reset();
    sectionArena_ = externalArena_;
  } else {
    if (crosspoint::heap::largestFreeBlockBytes() >= kLayoutScratchArenaBytes * 3) {
      layoutArena_ = crosspoint::layout::LayoutArena::create(kLayoutScratchArenaBytes);
    }
    sectionArena_ = &layoutArena_;
  }

  if (hyphenationEnabled) {
    Hyphenator::setPreferredLanguage("en");
  }
  auto paragraphAlignmentBlockStyle = BlockStyle();
  paragraphAlignmentBlockStyle.textAlignDefined = true;
  // Resolve None sentinel to Justify for initial block (no CSS context yet)
  const auto align = (this->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                         ? CssTextAlign::Justify
                         : static_cast<CssTextAlign>(this->paragraphAlignment);
  paragraphAlignmentBlockStyle.alignment = align;
  startNewTextBlock(paragraphAlignmentBlockStyle);

  const XML_Parser parser = XML_ParserCreate(nullptr);
  int done;

  if (!parser) {
    LOG_DIAG("EHP", "OOM XML_ParserCreate free=%u largest=%u min=%u fontId=%d", (unsigned)ESP.getFreeHeap(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap(), fontId);
    return false;
  }

  // Handle HTML entities (like &nbsp;) that aren't in XML spec or DTD
  // Using DefaultHandlerExpand preserves normal entity expansion from DOCTYPE
  XML_SetDefaultHandlerExpand(parser, defaultHandlerExpand);

  FsFile file;
  if (!Storage.openFileForRead("EHP", filepath, file)) {
    LOG_DIAG("EHP", "open temp HTML failed path=%s free=%u largest=%u min=%u", filepath.c_str(),
             (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)ESP.getMinFreeHeap());
    XML_ParserFree(parser);
    return false;
  }

  const size_t fileSize = file.size();
  const bool shouldReportProgress = progressFn && fileSize >= MIN_SIZE_FOR_PROGRESS;
  int lastReportedProgress = -1;
  if (shouldReportProgress) {
    progressFn(0);
    lastReportedProgress = 0;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  do {
    esp_task_wdt_reset();
    void* const buf = XML_GetBuffer(parser, 1024);
    if (!buf) {
      // Reset wdt before the cleanup-and-return branch so a slow SD on the
      // failure path doesn't compound an OOM with a watchdog reset.
      esp_task_wdt_reset();
      LOG_DIAG("EHP", "OOM XML_GetBuffer req=1024 free=%u largest=%u min=%u fontId=%d pos=%u/%u",
               (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
               (unsigned)ESP.getMinFreeHeap(), fontId, (unsigned)file.position(), (unsigned)fileSize);
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }

    const size_t len = file.read(buf, 1024);

    if (len == 0 && file.available() > 0) {
      LOG_DIAG("EHP", "File read error pos=%u/%u free=%u", (unsigned)file.position(), (unsigned)fileSize,
               (unsigned)ESP.getFreeHeap());
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }

    done = file.available() == 0;

    if (shouldReportProgress) {
      int progress = static_cast<int>((file.position() * 100U) / fileSize);
      if (done) {
        progress = 100;
      } else {
        progress = (progress / PROGRESS_STEP_PERCENT) * PROGRESS_STEP_PERCENT;
      }

      if (progress > lastReportedProgress) {
        progressFn(progress);
        lastReportedProgress = progress;
      }
    }

    if (XML_ParseBuffer(parser, static_cast<int>(len), done) == XML_STATUS_ERROR) {
      const auto errCode = XML_GetErrorCode(parser);
      LOG_DIAG("EHP", "XML parse error code=%d line=%lu free=%u largest=%u msg=%s", (int)errCode,
               XML_GetCurrentLineNumber(parser), (unsigned)ESP.getFreeHeap(),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), XML_ErrorString(errCode));
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }
    // A `new (nothrow) Page()` inside a callback may have set parseFailed.
    // expat callbacks are void-returning so the only way to short-circuit
    // is to poll the flag here and bail with the same cleanup as the
    // XML_GetBuffer / XML_ParseBuffer error paths.
    if (parseFailed) {
      XML_StopParser(parser, XML_FALSE);
      XML_SetElementHandler(parser, nullptr, nullptr);
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }
  } while (!done);

  XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
  XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
  XML_SetCharacterDataHandler(parser, nullptr);
  XML_ParserFree(parser);
  file.close();

  // Process last page if there is still text
  if (currentTextBlock) {
    makePages();
    currentTextBlock.reset();
  }

  if (!pendingAnchors.empty() && currentPage && !currentPage->elements.empty()) {
    bindPendingAnchorsToCurrentPage();
  }

  if (currentPage && !currentPage->elements.empty()) {
    completeCurrentPage();
    currentPage.reset();
  }

  // Final makePages() call above may have hit OOM after the parse loop
  // already exited cleanly. Surface the failure to the caller.
  if (parseFailed) {
    return false;
  }

  return true;
}

void ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line) {
  if (parseFailed) return;
  // The layout engine signals an OOM during TextBlock construction by passing
  // a null shared_ptr through the processLine callback. Treat that the
  // same as our own internal allocation failures.
  if (!line) {
    LOG_DIAG("EHP", "OOM upstream null TextBlock free=%u largest=%u min=%u fontId=%d", (unsigned)ESP.getFreeHeap(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap(), fontId);
    parseFailed = true;
    return;
  }
  const int baseLineHeight = renderer.getLineHeight(fontId) * lineCompression;
  const int lineHeight = line->getBlockStyle().resolveLineHeight(baseLineHeight);

  if (!currentPage) {
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_DIAG("EHP", "OOM new Page (addLineToPage entry) free=%u largest=%u min=%u fontId=%d",
               (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
               (unsigned)ESP.getMinFreeHeap(), fontId);
      parseFailed = true;
      return;
    }
    currentPageNextY = 0;
  }

  if (currentPageNextY + lineHeight > viewportHeight) {
    completeCurrentPage();
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_DIAG("EHP", "OOM new Page (addLineToPage overflow) free=%u largest=%u min=%u fontId=%d",
               (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
               (unsigned)ESP.getMinFreeHeap(), fontId);
      parseFailed = true;
      return;
    }
    currentPageNextY = 0;
  }

  bindPendingAnchorsToCurrentPage();

  // Track cumulative words to assign footnotes to the page containing their anchor
  footnotePlacer_.placeForLine(line->wordCount(),
                               [this](const char* number, const char* href) {
                                 currentPage->addFootnote(number, href);
                               });

  // Apply horizontal left inset (margin + padding) as x position offset
  const int16_t xOffset = line->getBlockStyle().leftInset();
  // Heap-probe then make_shared: probing keeps the throwing make_shared safe
  // under -fno-exceptions (kDefaultHeadroomBytes absorbs the control block +
  // transient growth) while fusing the PageLine object and its control block
  // into a single allocation — one fewer heap block per line than the old
  // `new (nothrow)` + `shared_ptr(raw)` form.
  if (!crosspoint::heap::canAllocateContiguous(sizeof(PageLine))) {
    LOG_DIAG("EHP", "OOM PageLine probe free=%u largest=%u min=%u fontId=%d", (unsigned)ESP.getFreeHeap(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap(), fontId);
    parseFailed = true;
    return;
  }
  currentPage->elements.push_back(std::make_shared<PageLine>(line, xOffset, currentPageNextY));
  currentPageNextY += lineHeight;
}

void ChapterHtmlSlimParser::makePages() {
  if (parseFailed) return;
  if (!currentTextBlock) {
    LOG_ERR("EHP", "!! No text block to make pages for !!");
    return;
  }

  if (!currentPage) {
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_DIAG("EHP", "OOM new Page (makePages entry) free=%u largest=%u min=%u fontId=%d", (unsigned)ESP.getFreeHeap(),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap(), fontId);
      parseFailed = true;
      return;
    }
    currentPageNextY = 0;
  }

  const int baseLineHeight = renderer.getLineHeight(fontId) * lineCompression;

  // Apply top spacing before the paragraph (stored in pixels)
  const BlockStyle& blockStyle = currentTextBlock->blockStyle();
  const int blockLineHeight = blockStyle.resolveLineHeight(baseLineHeight);
  if (blockStyle.marginTop > 0) {
    currentPageNextY += blockStyle.marginTop;
  }
  if (blockStyle.paddingTop > 0) {
    currentPageNextY += blockStyle.paddingTop;
  }

  // Calculate effective width accounting for horizontal margins/padding
  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;

  const crosspoint::layout::LayoutStatus status = currentTextBlock->flush(
      effectiveWidth, [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); });
  if (status != crosspoint::layout::LayoutStatus::Ok) {
    parseFailed = true;
    return;
  }

  // Fallback: transfer any remaining pending footnotes to current page.
  // Normally addLineToPage handles this via word-index tracking, but this catches
  // edge cases where a footnote's word index equals the exact block size.
  if (!footnotePlacer_.empty() && currentPage) {
    footnotePlacer_.drainRemaining(
        [this](const char* number, const char* href) { currentPage->addFootnote(number, href); });
  }

  // Apply bottom spacing after the paragraph (stored in pixels)
  if (blockStyle.marginBottom > 0) {
    currentPageNextY += blockStyle.marginBottom;
  }
  if (blockStyle.paddingBottom > 0) {
    currentPageNextY += blockStyle.paddingBottom;
  }

  // Additional spacing between paragraphs based on user-selected level.
  int extraParagraphGap = 0;
  switch (extraParagraphSpacingLevel) {
    case 1:  // S
      extraParagraphGap = blockLineHeight / 6;
      break;
    case 2:  // M
      extraParagraphGap = blockLineHeight / 4;
      break;
    case 3:  // L
      extraParagraphGap = blockLineHeight / 3;
      break;
    case 0:  // Off
    default:
      break;
  }
  if (extraParagraphGap > 0) {
    currentPageNextY += extraParagraphGap;
  }
}
