#include "MarkdownParser.h"

#include <HalStorage.h>
#include <HardwareSerial.h>

#include "md_parser.h"

namespace {
constexpr size_t READ_CHUNK_SIZE = 4 * 1024;  // 4KB chunks
constexpr size_t MIN_SIZE_FOR_POPUP = 10 * 1024;
}  // namespace

MarkdownParser::MarkdownParser(Markdown& markdown, GfxRenderer& renderer, const int fontId,
                               const float lineCompression, const bool extraParagraphSpacing,
                               const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                               const uint16_t viewportHeight, const bool hyphenationEnabled,
                               const std::function<void(std::unique_ptr<Page>)>& completePageFn,
                               const std::function<void()>& popupFn)
    : markdown(markdown),
      renderer(renderer),
      fontId(fontId),
      lineCompression(lineCompression),
      extraParagraphSpacing(extraParagraphSpacing),
      paragraphAlignment(paragraphAlignment),
      viewportWidth(viewportWidth),
      viewportHeight(viewportHeight),
      hyphenationEnabled(hyphenationEnabled),
      completePageFn(completePageFn),
      popupFn(popupFn) {}

EpdFontFamily::Style MarkdownParser::currentFontStyle() const {
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  if (inBold || inHeader) {
    style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD);
  }
  if (inItalic) {
    style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::ITALIC);
  }
  return style;
}

BlockStyle MarkdownParser::headerBlockStyle(int level) const {
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  style.textAlignDefined = true;
  // Scale margins by header level
  const int lineHeight = renderer.getLineHeight(fontId);
  style.marginTop = static_cast<int16_t>(lineHeight * (level <= 2 ? 1.0f : 0.5f));
  style.marginBottom = static_cast<int16_t>(lineHeight * 0.3f);
  return style;
}

BlockStyle MarkdownParser::blockquoteBlockStyle() const {
  BlockStyle style;
  style.alignment = static_cast<CssTextAlign>(paragraphAlignment);
  style.textAlignDefined = true;
  style.marginLeft = 15;
  style.paddingLeft = 5;
  return style;
}

BlockStyle MarkdownParser::paragraphBlockStyle() const {
  BlockStyle style;
  const auto align = (paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None)) ? CssTextAlign::Justify
                                                                                       : static_cast<CssTextAlign>(paragraphAlignment);
  style.alignment = align;
  style.textAlignDefined = true;
  return style;
}

BlockStyle MarkdownParser::listItemBlockStyle(int level, const std::string& prefix) const {
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  style.textAlignDefined = true;
  const int16_t margin = static_cast<int16_t>(15 * (level + 1));
  style.marginLeft = margin;
  style.hangingIndent = static_cast<int16_t>(margin + renderer.getTextWidth(fontId, prefix.c_str()));
  return style;
}

void MarkdownParser::startNewTextBlock(const BlockStyle& blockStyle) {
  if (currentTextBlock) {
    if (currentTextBlock->isEmpty()) {
      currentTextBlock->setBlockStyle(currentTextBlock->getBlockStyle().getCombinedBlockStyle(blockStyle));
      return;
    }
    makePages();
  }
  currentTextBlock.reset(new ParsedText(extraParagraphSpacing, hyphenationEnabled, blockStyle));
}

void MarkdownParser::addLineToPage(std::shared_ptr<TextBlock> line) {
  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;

  if (currentPageNextY + lineHeight > viewportHeight) {
    completePageFn(std::move(currentPage));
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  const int16_t xOffset = line->getBlockStyle().leftInset();
  currentPage->elements.push_back(std::make_shared<PageLine>(line, xOffset, currentPageNextY));
  currentPageNextY += lineHeight;
}

void MarkdownParser::makePages() {
  if (!currentTextBlock) {
    return;
  }

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;
  const BlockStyle& blockStyle = currentTextBlock->getBlockStyle();

  if (blockStyle.marginTop > 0) {
    currentPageNextY += blockStyle.marginTop;
  }
  if (blockStyle.paddingTop > 0) {
    currentPageNextY += blockStyle.paddingTop;
  }

  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;

  Serial.printf("[%lu] [MDP] makePages: %d words, width=%d\n", millis(), currentTextBlock->size(), effectiveWidth);

  currentTextBlock->layoutAndExtractLines(
      renderer, fontId, effectiveWidth,
      [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); });

  Serial.printf("[%lu] [MDP] makePages complete\n", millis());

  if (blockStyle.marginBottom > 0) {
    currentPageNextY += blockStyle.marginBottom;
  }
  if (blockStyle.paddingBottom > 0) {
    currentPageNextY += blockStyle.paddingBottom;
  }

  if (extraParagraphSpacing) {
    currentPageNextY += lineHeight / 2;
  }
}

// Token callback - bridges C tokenizer to C++ page builder
namespace {
struct ParserContext {
  MarkdownParser* parser;
};
}  // namespace

void mdTokenCallback(const md_token* token, void* user_data);

bool MarkdownParser::parseAndBuildPages() {
  startNewTextBlock(paragraphBlockStyle());

  if (popupFn && markdown.getFileSize() >= MIN_SIZE_FOR_POPUP) {
    popupFn();
  }

  md_parser_state parserState;
  md_parser_init(&parserState, mdTokenCallback, this);

  // Read file in chunks, split into lines, feed to parser
  const size_t fileSize = markdown.getFileSize();
  auto* buffer = static_cast<uint8_t*>(malloc(READ_CHUNK_SIZE + 1));
  if (!buffer) {
    Serial.printf("[%lu] [MDP] Failed to allocate read buffer\n", millis());
    return false;
  }

  std::string lineBuffer;
  size_t offset = 0;

  while (offset < fileSize) {
    size_t chunkSize = std::min(READ_CHUNK_SIZE, fileSize - offset);
    if (!markdown.readContent(buffer, offset, chunkSize)) {
      Serial.printf("[%lu] [MDP] Failed to read content at offset %zu\n", millis(), offset);
      break;
    }
    buffer[chunkSize] = '\0';
    offset += chunkSize;
    Serial.printf("[%lu] [MDP] Read chunk: offset=%zu/%zu\n", millis(), offset, fileSize);

    // Split into lines
    int lineNum = 0;
    for (size_t i = 0; i < chunkSize; i++) {
      if (buffer[i] == '\n') {
        Serial.printf("[%lu] [MDP] parse_line #%d (%zu chars)\n", millis(), lineNum, lineBuffer.size());
        md_parse_line(&parserState, lineBuffer.c_str(), lineBuffer.size());
        Serial.printf("[%lu] [MDP] parse_line #%d done\n", millis(), lineNum);
        lineNum++;
        lineBuffer.clear();
      } else if (buffer[i] != '\r') {
        lineBuffer += static_cast<char>(buffer[i]);
      }
    }
    Serial.printf("[%lu] [MDP] Chunk parsed (%d lines)\n", millis(), lineNum);
  }

  Serial.printf("[%lu] [MDP] All chunks read, finishing\n", millis());

  // Process any remaining text in the line buffer
  if (!lineBuffer.empty()) {
    Serial.printf("[%lu] [MDP] Processing remaining line buffer (%zu chars)\n", millis(), lineBuffer.size());
    md_parse_line(&parserState, lineBuffer.c_str(), lineBuffer.size());
    Serial.printf("[%lu] [MDP] Remaining line done\n", millis());
  }

  md_parser_finish(&parserState);
  free(buffer);

  Serial.printf("[%lu] [MDP] Parser finished, flushing remaining text\n", millis());

  // Flush remaining text
  if (currentTextBlock && !currentTextBlock->isEmpty()) {
    Serial.printf("[%lu] [MDP] Flushing final block (%zu words)\n", millis(), currentTextBlock->size());
    makePages();
  }
  if (currentPage) {
    completePageFn(std::move(currentPage));
    currentPage.reset();
  }

  return true;
}

void mdTokenCallback(const md_token* token, void* user_data) {
  auto* self = static_cast<MarkdownParser*>(user_data);

  // Extract text as std::string when needed
  auto getText = [&]() -> std::string {
    if (token->text && token->text_len > 0) {
      return std::string(token->text, token->text_len);
    }
    return "";
  };

  switch (token->type) {
    case MD_TOKEN_HEADER_START:
      self->inHeader = true;
      self->headerLevel = token->level;
      self->startNewTextBlock(self->headerBlockStyle(token->level));
      break;

    case MD_TOKEN_HEADER_END:
      self->inHeader = false;
      self->headerLevel = 0;
      break;

    case MD_TOKEN_BOLD_START:
      self->inBold = true;
      break;

    case MD_TOKEN_BOLD_END:
      self->inBold = false;
      break;

    case MD_TOKEN_ITALIC_START:
      self->inItalic = true;
      break;

    case MD_TOKEN_ITALIC_END:
      self->inItalic = false;
      break;

    case MD_TOKEN_BOLD_ITALIC_START:
      self->inBold = true;
      self->inItalic = true;
      break;

    case MD_TOKEN_BOLD_ITALIC_END:
      self->inBold = false;
      self->inItalic = false;
      break;

    case MD_TOKEN_TEXT: {
      std::string text = getText();
      if (text.empty()) break;

      // Split on spaces and add words
      size_t i = 0;
      while (i < text.size()) {
        // Skip whitespace
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) i++;
        if (i >= text.size()) break;

        // Find end of word
        size_t start = i;
        while (i < text.size() && text[i] != ' ' && text[i] != '\t') i++;

        std::string word = text.substr(start, i - start);
        if (self->currentTextBlock) {
          self->currentTextBlock->addWord(word, self->currentFontStyle());
        }
      }
      break;
    }

    case MD_TOKEN_CODE_SPAN: {
      std::string text = getText();
      if (!text.empty() && self->currentTextBlock) {
        // Code spans render as regular text (no monospace on e-reader)
        self->currentTextBlock->addWord(text, EpdFontFamily::REGULAR);
      }
      break;
    }

    case MD_TOKEN_LINK_TEXT: {
      // Render link text in italic
      std::string text = getText();
      if (!text.empty() && self->currentTextBlock) {
        // Split on spaces
        size_t i = 0;
        while (i < text.size()) {
          while (i < text.size() && text[i] == ' ') i++;
          if (i >= text.size()) break;
          size_t start = i;
          while (i < text.size() && text[i] != ' ') i++;
          EpdFontFamily::Style style = self->currentFontStyle();
          self->currentTextBlock->addWord(text.substr(start, i - start), style, true);
        }
      }
      break;
    }

    case MD_TOKEN_LINK_URL:
      // Skip URL text in output — just show the link text
      break;

    case MD_TOKEN_LIST_ITEM:
      self->startNewTextBlock(self->listItemBlockStyle(token->level, "\xe2\x80\xa2 "));  // "• "
      if (self->currentTextBlock) {
        self->currentTextBlock->addWord("\xe2\x80\xa2", EpdFontFamily::REGULAR);  // bullet •
      }
      break;

    case MD_TOKEN_ORDERED_ITEM: {
      std::string number(token->text, token->text_len);
      std::string prefix = number + ". ";
      self->startNewTextBlock(self->listItemBlockStyle(token->level, prefix));
      if (self->currentTextBlock) {
        self->currentTextBlock->addWord(number + ".", EpdFontFamily::REGULAR);
      }
      break;
    }

    case MD_TOKEN_BLOCKQUOTE_START:
      self->inBlockquote = true;
      self->startNewTextBlock(self->blockquoteBlockStyle());
      break;

    case MD_TOKEN_BLOCKQUOTE_END:
      self->inBlockquote = false;
      break;

    case MD_TOKEN_HORIZONTAL_RULE: {
      self->makePages();  // flush pending text
      if (!self->currentPage) {
        self->currentPage.reset(new Page());
        self->currentPageNextY = 0;
      }
      const int lineHeight = self->renderer.getLineHeight(self->fontId);
      const int yMid = self->currentPageNextY + lineHeight / 2;
      const int sepWidth = self->viewportWidth * 80 / 100;
      const int sepX = (self->viewportWidth - sepWidth) / 2;
      self->currentPage->elements.push_back(
          std::make_shared<PageSeparator>(sepX, yMid, sepWidth));
      self->currentPageNextY += lineHeight;
      break;
    }

    case MD_TOKEN_PARAGRAPH_BREAK: {
      // Flush current block, add half-line spacing, then start new paragraph
      bool hadContent = self->currentTextBlock && !self->currentTextBlock->isEmpty();
      if (self->inBlockquote) {
        self->startNewTextBlock(self->blockquoteBlockStyle());
      } else {
        self->startNewTextBlock(self->paragraphBlockStyle());
      }
      if (hadContent) {
        self->currentPageNextY += self->renderer.getLineHeight(self->fontId) / 2;
      }
      break;
    }

    case MD_TOKEN_LINE_BREAK:
      // Soft line break — whitespace continuation, ParsedText handles wrapping.
      break;

    case MD_TOKEN_HARD_LINE_BREAK:
      // Hard line break (trailing spaces) — force new line, same block style, no paragraph spacing.
      if (self->currentTextBlock) {
        BlockStyle style = self->currentTextBlock->getBlockStyle();
        self->makePages();
        self->startNewTextBlock(style);
      }
      break;

    case MD_TOKEN_CODE_BLOCK_START:
      self->startNewTextBlock(self->paragraphBlockStyle());
      break;

    case MD_TOKEN_CODE_BLOCK_LINE: {
      std::string text = getText();
      if (self->currentTextBlock) {
        if (!text.empty()) {
          // Add as a single "word" to preserve spacing
          self->currentTextBlock->addWord(text, EpdFontFamily::REGULAR);
        }
        // Force line break by flushing current block
        self->makePages();
        self->startNewTextBlock(self->paragraphBlockStyle());
      }
      break;
    }

    case MD_TOKEN_CODE_BLOCK_END:
      break;
  }
}
