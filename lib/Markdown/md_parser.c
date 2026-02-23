#include "md_parser.h"

#include <string.h>

/* ---------- helpers ---------- */

static void emit(md_parser_state* s, md_token_type type, const char* text, size_t len, int level) {
  md_token tok;
  tok.type = type;
  tok.text = text;
  tok.text_len = len;
  tok.level = level;
  s->callback(&tok, s->user_data);
}

static void emit_simple(md_parser_state* s, md_token_type type) { emit(s, type, NULL, 0, 0); }

static int starts_with(const char* s, size_t len, const char* prefix) {
  size_t plen = strlen(prefix);
  if (len < plen) return 0;
  return memcmp(s, prefix, plen) == 0;
}

/* Skip leading whitespace, return number of spaces skipped */
static size_t skip_spaces(const char* s, size_t len) {
  size_t i = 0;
  while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
  return i;
}

/* Check if line is a horizontal rule: 3+ of same char (-, *, _) with optional spaces */
static int is_horizontal_rule(const char* line, size_t len) {
  size_t i = skip_spaces(line, len);
  if (i >= len) return 0;

  char ch = line[i];
  if (ch != '-' && ch != '*' && ch != '_') return 0;

  int count = 0;
  while (i < len) {
    if (line[i] == ch)
      count++;
    else if (line[i] != ' ' && line[i] != '\t')
      return 0;
    i++;
  }
  return count >= 3;
}

/* Check if line is a fenced code block delimiter (``` or ~~~) */
static int is_code_fence(const char* line, size_t len) {
  size_t i = skip_spaces(line, len);
  if (i >= len) return 0;

  char ch = line[i];
  if (ch != '`' && ch != '~') return 0;

  int count = 0;
  size_t start = i;
  while (i < len && line[i] == ch) {
    count++;
    i++;
  }
  return count >= 3;
}

/* Count ATX header level (# to ######), return 0 if not a header */
static int atx_header_level(const char* line, size_t len) {
  size_t i = skip_spaces(line, len);
  if (i >= len || line[i] != '#') return 0;

  int level = 0;
  while (i < len && line[i] == '#' && level < 6) {
    level++;
    i++;
  }
  /* Must be followed by space or end of line */
  if (i < len && line[i] != ' ' && line[i] != '\t') return 0;
  return level;
}

/* Strip trailing # from header text */
static size_t strip_trailing_hashes(const char* text, size_t len) {
  while (len > 0 && text[len - 1] == '#') len--;
  while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) len--;
  return len;
}

/* Check for hard line break (2+ trailing spaces). Returns stripped length, sets *hard_break. */
static size_t check_hard_break(const char* text, size_t len, int* hard_break) {
  *hard_break = (len >= 2 && text[len - 1] == ' ' && text[len - 2] == ' ') ? 1 : 0;
  if (*hard_break) {
    while (len > 0 && text[len - 1] == ' ') len--;
  }
  return len;
}

/* Check if a line is a list item. Returns the content offset or 0 if not a list.
 * Sets *ordered to 1 for ordered lists, 0 for unordered.
 * Sets *indent_level based on leading whitespace. */
static size_t is_list_item(const char* line, size_t len, int* ordered, int* indent_level,
                           const char** num_text, size_t* num_len) {
  size_t i = skip_spaces(line, len);
  *indent_level = (int)(i / 2); /* 2 spaces = 1 indent level */
  *num_text = NULL;
  *num_len = 0;

  if (i >= len) return 0;

  /* Unordered: -, *, + followed by space */
  if ((line[i] == '-' || line[i] == '*' || line[i] == '+') && i + 1 < len && line[i + 1] == ' ') {
    *ordered = 0;
    return i + 2;
  }

  /* Ordered: digits followed by . or ) and space */
  size_t num_start = i;
  while (i < len && line[i] >= '0' && line[i] <= '9') i++;
  if (i > num_start && i < len && (line[i] == '.' || line[i] == ')') && i + 1 < len && line[i + 1] == ' ') {
    *ordered = 1;
    *num_text = line + num_start;
    *num_len = i - num_start;
    return i + 2;
  }

  return 0;
}

/* Count blockquote nesting level (> > > text) */
static size_t strip_blockquote(const char* line, size_t len, int* bq_level) {
  size_t i = 0;
  *bq_level = 0;
  while (i < len) {
    size_t j = i;
    while (j < len && (line[j] == ' ' || line[j] == '\t')) j++;
    if (j < len && line[j] == '>') {
      (*bq_level)++;
      j++;
      if (j < len && line[j] == ' ') j++;
      i = j;
    } else {
      break;
    }
  }
  return i;
}

/* ---------- inline parsing ---------- */

/*
 * Parse inline markdown formatting within a text span.
 * Handles: **bold**, *italic*, ***bold italic***, `code`, [links](url)
 */
static void parse_inline(md_parser_state* s, const char* text, size_t len) {
  size_t i = 0;

  while (i < len) {
    /* Inline code */
    if (text[i] == '`') {
      size_t end = i + 1;
      while (end < len && text[end] != '`') end++;
      if (end < len) {
        emit(s, MD_TOKEN_CODE_SPAN, text + i + 1, end - i - 1, 0);
        i = end + 1;
        continue;
      }
      /* No closing backtick — emit as text */
      emit(s, MD_TOKEN_TEXT, text + i, 1, 0);
      i++;
      continue;
    }

    /* Link: [text](url) */
    if (text[i] == '[') {
      size_t bracket_end = i + 1;
      while (bracket_end < len && text[bracket_end] != ']') bracket_end++;
      if (bracket_end < len && bracket_end + 1 < len && text[bracket_end + 1] == '(') {
        size_t paren_end = bracket_end + 2;
        while (paren_end < len && text[paren_end] != ')') paren_end++;
        if (paren_end < len) {
          emit(s, MD_TOKEN_LINK_TEXT, text + i + 1, bracket_end - i - 1, 0);
          emit(s, MD_TOKEN_LINK_URL, text + bracket_end + 2, paren_end - bracket_end - 2, 0);
          i = paren_end + 1;
          continue;
        }
      }
      /* Not a valid link — emit '[' as text */
      emit(s, MD_TOKEN_TEXT, text + i, 1, 0);
      i++;
      continue;
    }

    /* Bold/italic markers: ***, **, *, ___, __, _ */
    if (text[i] == '*' || text[i] == '_') {
      char marker = text[i];
      int count = 0;
      size_t mark_start = i;
      while (i < len && text[i] == marker && count < 3) {
        count++;
        i++;
      }

      if (count == 3) {
        /* Find matching closing *** */
        size_t end = i;
        while (end + 2 < len) {
          if (text[end] == marker && text[end + 1] == marker && text[end + 2] == marker) {
            emit_simple(s, MD_TOKEN_BOLD_ITALIC_START);
            parse_inline(s, text + i, end - i);
            emit_simple(s, MD_TOKEN_BOLD_ITALIC_END);
            i = end + 3;
            goto next_char;
          }
          end++;
        }
        /* No match found, emit as text */
        emit(s, MD_TOKEN_TEXT, text + mark_start, count, 0);
        continue;
      }

      if (count == 2) {
        /* Find matching closing ** */
        size_t end = i;
        while (end + 1 < len) {
          if (text[end] == marker && text[end + 1] == marker) {
            emit_simple(s, MD_TOKEN_BOLD_START);
            parse_inline(s, text + i, end - i);
            emit_simple(s, MD_TOKEN_BOLD_END);
            i = end + 2;
            goto next_char;
          }
          end++;
        }
        /* No match found, emit as text */
        emit(s, MD_TOKEN_TEXT, text + mark_start, count, 0);
        continue;
      }

      if (count == 1) {
        /* Find matching closing * */
        size_t end = i;
        while (end < len) {
          if (text[end] == marker) {
            emit_simple(s, MD_TOKEN_ITALIC_START);
            parse_inline(s, text + i, end - i);
            emit_simple(s, MD_TOKEN_ITALIC_END);
            i = end + 1;
            goto next_char;
          }
          end++;
        }
        /* No match found, emit as text */
        emit(s, MD_TOKEN_TEXT, text + mark_start, 1, 0);
        continue;
      }
    }

    /* Plain text: scan forward to next special char */
    {
      size_t start = i;
      while (i < len && text[i] != '*' && text[i] != '_' && text[i] != '`' && text[i] != '[') {
        i++;
      }
      if (i > start) {
        emit(s, MD_TOKEN_TEXT, text + start, i - start, 0);
      }
    }

  next_char:;
  }
}

/* ---------- public API ---------- */

void md_parser_init(md_parser_state* state, md_token_callback callback, void* user_data) {
  memset(state, 0, sizeof(*state));
  state->callback = callback;
  state->user_data = user_data;
}

void md_parse_line(md_parser_state* state, const char* line, size_t len) {
  /* Strip trailing \r\n */
  while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) len--;

  /* Inside fenced code block */
  if (state->in_code_block) {
    if (is_code_fence(line, len)) {
      emit_simple(state, MD_TOKEN_CODE_BLOCK_END);
      state->in_code_block = 0;
    } else {
      emit(state, MD_TOKEN_CODE_BLOCK_LINE, line, len, 0);
    }
    return;
  }

  /* Fenced code block start */
  if (is_code_fence(line, len)) {
    emit_simple(state, MD_TOKEN_CODE_BLOCK_START);
    state->in_code_block = 1;
    state->last_line_was_blank = 0;
    return;
  }

  /* Strip blockquote markers */
  int bq_level = 0;
  size_t bq_offset = strip_blockquote(line, len, &bq_level);

  /* Adjust blockquote nesting */
  while (state->in_blockquote < bq_level) {
    emit_simple(state, MD_TOKEN_BLOCKQUOTE_START);
    state->in_blockquote++;
  }
  while (state->in_blockquote > bq_level) {
    emit_simple(state, MD_TOKEN_BLOCKQUOTE_END);
    state->in_blockquote--;
  }

  const char* content = line + bq_offset;
  size_t content_len = len - bq_offset;

  /* Blank line */
  size_t spaces = skip_spaces(content, content_len);
  if (spaces == content_len) {
    if (!state->last_line_was_blank) {
      emit_simple(state, MD_TOKEN_PARAGRAPH_BREAK);
    }
    state->last_line_was_blank = 1;
    return;
  }
  state->last_line_was_blank = 0;

  /* Horizontal rule */
  if (is_horizontal_rule(content, content_len)) {
    emit_simple(state, MD_TOKEN_HORIZONTAL_RULE);
    return;
  }

  /* ATX header */
  int h_level = atx_header_level(content, content_len);
  if (h_level > 0) {
    /* Skip # and space */
    size_t h_start = 0;
    while (h_start < content_len && content[h_start] == '#') h_start++;
    while (h_start < content_len && content[h_start] == ' ') h_start++;
    size_t h_text_len = strip_trailing_hashes(content + h_start, content_len - h_start);

    emit(state, MD_TOKEN_HEADER_START, NULL, 0, h_level);
    parse_inline(state, content + h_start, h_text_len);
    emit(state, MD_TOKEN_HEADER_END, NULL, 0, h_level);
    return;
  }

  /* List item */
  int ordered = 0, indent_level = 0;
  const char* num_text = NULL;
  size_t num_len = 0;
  size_t list_content_offset = is_list_item(content, content_len, &ordered, &indent_level, &num_text, &num_len);
  if (list_content_offset > 0) {
    if (ordered) {
      emit(state, MD_TOKEN_ORDERED_ITEM, num_text, num_len, indent_level);
    } else {
      emit(state, MD_TOKEN_LIST_ITEM, NULL, 0, indent_level);
    }
    parse_inline(state, content + list_content_offset, content_len - list_content_offset);
    emit_simple(state, MD_TOKEN_LINE_BREAK);
    return;
  }

  /* Normal text line — detect hard line break (2+ trailing spaces) */
  int hard_break = 0;
  size_t inline_len = check_hard_break(content, content_len, &hard_break);
  parse_inline(state, content, inline_len);
  emit_simple(state, hard_break ? MD_TOKEN_HARD_LINE_BREAK : MD_TOKEN_LINE_BREAK);
}

void md_parser_finish(md_parser_state* state) {
  if (state->in_code_block) {
    emit_simple(state, MD_TOKEN_CODE_BLOCK_END);
    state->in_code_block = 0;
  }
  while (state->in_blockquote > 0) {
    emit_simple(state, MD_TOKEN_BLOCKQUOTE_END);
    state->in_blockquote--;
  }
}
