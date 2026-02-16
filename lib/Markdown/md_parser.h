#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/**
 * Lightweight streaming Markdown tokenizer.
 *
 * Feed it lines of text via md_parse_line(). For each line, it emits tokens
 * through the callback describing the structure (headers, bold, italic,
 * lists, blockquotes, code, links, horizontal rules, and plain text).
 *
 * Designed for embedded use — no heap allocation, no external dependencies.
 */

typedef enum {
  MD_TOKEN_TEXT,           // plain text fragment
  MD_TOKEN_HEADER_START,   // header begins; level in md_token.level (1-6)
  MD_TOKEN_HEADER_END,     // header ends
  MD_TOKEN_BOLD_START,     // ** or __
  MD_TOKEN_BOLD_END,
  MD_TOKEN_ITALIC_START,   // * or _
  MD_TOKEN_ITALIC_END,
  MD_TOKEN_BOLD_ITALIC_START,  // *** or ___
  MD_TOKEN_BOLD_ITALIC_END,
  MD_TOKEN_CODE_SPAN,     // inline `code`; text in md_token.text
  MD_TOKEN_LINK_TEXT,      // [text] portion of a link
  MD_TOKEN_LINK_URL,       // (url) portion of a link
  MD_TOKEN_LIST_ITEM,      // list item bullet; md_token.level = nesting depth (0-based)
  MD_TOKEN_ORDERED_ITEM,   // ordered list item; md_token.level = nesting, number in text
  MD_TOKEN_BLOCKQUOTE_START,
  MD_TOKEN_BLOCKQUOTE_END,
  MD_TOKEN_HORIZONTAL_RULE,
  MD_TOKEN_PARAGRAPH_BREAK, // blank line / paragraph boundary
  MD_TOKEN_LINE_BREAK,      // end of a non-blank line within a block
  MD_TOKEN_CODE_BLOCK_START,
  MD_TOKEN_CODE_BLOCK_END,
  MD_TOKEN_CODE_BLOCK_LINE,
} md_token_type;

typedef struct {
  md_token_type type;
  const char* text;    // pointer into source buffer (not null-terminated in general)
  size_t text_len;     // length of text
  int level;           // header level (1-6) or list nesting depth
} md_token;

typedef void (*md_token_callback)(const md_token* token, void* user_data);

typedef struct {
  md_token_callback callback;
  void* user_data;
  int in_code_block;        // 1 if inside ``` fenced code block
  int in_blockquote;        // blockquote nesting depth
  int last_line_was_blank;  // for paragraph break detection
} md_parser_state;

/**
 * Initialize parser state. Call once before feeding lines.
 */
void md_parser_init(md_parser_state* state, md_token_callback callback, void* user_data);

/**
 * Parse a single line of markdown text.
 * @param state Parser state
 * @param line  Pointer to line text (does not need to be null-terminated)
 * @param len   Length of line in bytes (excluding any trailing \n or \r)
 */
void md_parse_line(md_parser_state* state, const char* line, size_t len);

/**
 * Finalize parsing — close any open blocks.
 */
void md_parser_finish(md_parser_state* state);

#ifdef __cplusplus
}
#endif
