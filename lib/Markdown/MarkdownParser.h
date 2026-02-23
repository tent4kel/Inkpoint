#pragma once

#include <EpdFontFamily.h>
#include <GfxRenderer.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Epub/Page.h"
#include "Epub/ParsedText.h"
#include "Epub/blocks/BlockStyle.h"
#include "Markdown.h"
#include "md_parser.h"

/**
 * MarkdownParser - Converts a Markdown file into styled Page objects
 * using the existing EPUB rendering pipeline (ParsedText → TextBlock → Page).
 *
 * This is analogous to ChapterHtmlSlimParser but for Markdown content.
 * It streams the .md file line-by-line, tokenizes with md_parser,
 * and builds Pages that can be cached and rendered identically to EPUB pages.
 */
class MarkdownParser {
  Markdown& markdown;
  GfxRenderer& renderer;
  int fontId;
  float lineCompression;
  bool extraParagraphSpacing;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  std::function<void(std::unique_ptr<Page>)> completePageFn;
  std::function<void()> popupFn;

  // Current state
  std::unique_ptr<ParsedText> currentTextBlock;
  std::unique_ptr<Page> currentPage;
  int16_t currentPageNextY = 0;

  // Inline style state
  bool inBold = false;
  bool inItalic = false;
  bool inHeader = false;
  int headerLevel = 0;
  bool inBlockquote = false;

  void startNewTextBlock(const BlockStyle& blockStyle);
  void makePages();
  void addLineToPage(std::shared_ptr<TextBlock> line);
  EpdFontFamily::Style currentFontStyle() const;
  BlockStyle headerBlockStyle(int level) const;
  BlockStyle blockquoteBlockStyle() const;
  BlockStyle paragraphBlockStyle() const;
  BlockStyle listItemBlockStyle(int level, const std::string& prefix) const;

  friend void mdTokenCallback(const md_token* token, void* user_data);

 public:
  MarkdownParser(Markdown& markdown, GfxRenderer& renderer, int fontId, float lineCompression,
                 bool extraParagraphSpacing, uint8_t paragraphAlignment, uint16_t viewportWidth,
                 uint16_t viewportHeight, bool hyphenationEnabled,
                 const std::function<void(std::unique_ptr<Page>)>& completePageFn,
                 const std::function<void()>& popupFn = nullptr);

  bool parseAndBuildPages();
};
