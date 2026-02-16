#include "HtmlToMarkdown.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {

// Case-insensitive tag comparison
bool tagEquals(const std::string& tag, const char* name) {
  if (tag.size() != strlen(name)) return false;
  for (size_t i = 0; i < tag.size(); i++) {
    if (tolower(tag[i]) != tolower(name[i])) return false;
  }
  return true;
}

bool tagStartsWith(const std::string& tag, const char* prefix) {
  size_t len = strlen(prefix);
  if (tag.size() < len) return false;
  for (size_t i = 0; i < len; i++) {
    if (tolower(tag[i]) != tolower(prefix[i])) return false;
  }
  return true;
}

// Decode HTML entity at position, returns decoded char count (0 if not an entity)
std::string decodeEntity(const std::string& html, size_t pos, size_t& consumed) {
  if (pos >= html.size() || html[pos] != '&') {
    consumed = 0;
    return "";
  }

  size_t end = html.find(';', pos);
  if (end == std::string::npos || end - pos > 10) {
    consumed = 0;
    return "";
  }

  std::string entity = html.substr(pos + 1, end - pos - 1);
  consumed = end - pos + 1;

  // Named entities
  if (entity == "amp") return "&";
  if (entity == "lt") return "<";
  if (entity == "gt") return ">";
  if (entity == "quot") return "\"";
  if (entity == "apos") return "'";
  if (entity == "nbsp") return " ";
  if (entity == "mdash" || entity == "#8212") return "--";
  if (entity == "ndash" || entity == "#8211") return "-";
  if (entity == "lsquo" || entity == "#8216") return "'";
  if (entity == "rsquo" || entity == "#8217") return "'";
  if (entity == "ldquo" || entity == "#8220") return "\"";
  if (entity == "rdquo" || entity == "#8221") return "\"";
  if (entity == "hellip" || entity == "#8230") return "...";

  // Numeric entities
  if (entity.size() > 1 && entity[0] == '#') {
    int codepoint = 0;
    if (entity[1] == 'x' || entity[1] == 'X') {
      codepoint = strtol(entity.c_str() + 2, nullptr, 16);
    } else {
      codepoint = atoi(entity.c_str() + 1);
    }
    if (codepoint > 0 && codepoint < 128) {
      return std::string(1, static_cast<char>(codepoint));
    }
    // For non-ASCII, try UTF-8 encoding
    if (codepoint > 0) {
      std::string utf8;
      if (codepoint < 0x80) {
        utf8 += static_cast<char>(codepoint);
      } else if (codepoint < 0x800) {
        utf8 += static_cast<char>(0xC0 | (codepoint >> 6));
        utf8 += static_cast<char>(0x80 | (codepoint & 0x3F));
      } else if (codepoint < 0x10000) {
        utf8 += static_cast<char>(0xE0 | (codepoint >> 12));
        utf8 += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        utf8 += static_cast<char>(0x80 | (codepoint & 0x3F));
      }
      return utf8;
    }
  }

  consumed = 0;
  return "";
}

// Extract href attribute from tag content like: a href="http://example.com" class="..."
std::string extractHref(const std::string& tagContent) {
  std::string lower = tagContent;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

  size_t pos = lower.find("href=");
  if (pos == std::string::npos) return "";

  pos += 5;
  if (pos >= tagContent.size()) return "";

  char quote = tagContent[pos];
  if (quote == '"' || quote == '\'') {
    pos++;
    size_t end = tagContent.find(quote, pos);
    if (end == std::string::npos) return "";
    return tagContent.substr(pos, end - pos);
  }

  // Unquoted
  size_t end = tagContent.find_first_of(" >", pos);
  if (end == std::string::npos) end = tagContent.size();
  return tagContent.substr(pos, end - pos);
}

}  // namespace

std::string HtmlToMarkdown::convert(const std::string& html) {
  std::string out;
  out.reserve(html.size() / 2);

  enum State { TEXT, TAG, SKIP_CONTENT } state = TEXT;

  std::string tagContent;
  std::string linkHref;
  std::string linkText;
  bool inLink = false;
  bool inBold = false;
  bool inItalic = false;
  bool inBlockquote = false;
  bool skipUntilClose = false;
  std::string skipTag;
  bool lastWasNewline = true;  // Start as if we just had a newline

  size_t i = 0;
  while (i < html.size()) {
    char c = html[i];

    if (state == SKIP_CONTENT) {
      // Skip <script> and <style> block contents
      if (c == '<') {
        size_t end = html.find('>', i);
        if (end != std::string::npos) {
          std::string tag = html.substr(i + 1, end - i - 1);
          // Remove leading /
          if (!tag.empty() && tag[0] == '/') {
            tag = tag.substr(1);
            // Remove attributes
            size_t sp = tag.find(' ');
            if (sp != std::string::npos) tag = tag.substr(0, sp);
            if (tagEquals(tag, skipTag.c_str())) {
              state = TEXT;
            }
          }
          i = end + 1;
          continue;
        }
      }
      i++;
      continue;
    }

    if (state == TAG) {
      if (c == '>') {
        state = TEXT;

        // Process the tag
        bool isClosing = !tagContent.empty() && tagContent[0] == '/';
        std::string rawTag = tagContent;
        if (isClosing) rawTag = rawTag.substr(1);

        // Remove attributes to get tag name
        size_t sp = rawTag.find(' ');
        std::string tagName = (sp != std::string::npos) ? rawTag.substr(0, sp) : rawTag;

        // Remove trailing / for self-closing tags
        if (!tagName.empty() && tagName.back() == '/') tagName.pop_back();

        // Convert to lowercase for comparison
        std::transform(tagName.begin(), tagName.end(), tagName.begin(), ::tolower);

        // Skip script/style blocks
        if (!isClosing && (tagName == "script" || tagName == "style")) {
          state = SKIP_CONTENT;
          skipTag = tagName;
        }
        // Headers
        else if (tagName.size() == 2 && tagName[0] == 'h' && tagName[1] >= '1' && tagName[1] <= '6') {
          if (!isClosing) {
            if (!lastWasNewline) out += "\n\n";
            int level = tagName[1] - '0';
            for (int h = 0; h < level; h++) out += '#';
            out += ' ';
            lastWasNewline = false;
          } else {
            out += "\n\n";
            lastWasNewline = true;
          }
        }
        // Paragraphs
        else if (tagName == "p" || tagName == "div") {
          if (isClosing) {
            out += "\n\n";
            lastWasNewline = true;
          } else if (!lastWasNewline) {
            out += "\n\n";
            lastWasNewline = true;
          }
        }
        // Bold
        else if (tagName == "strong" || tagName == "b") {
          out += "**";
          inBold = !isClosing;
          lastWasNewline = false;
        }
        // Italic
        else if (tagName == "em" || tagName == "i") {
          out += "*";
          inItalic = !isClosing;
          lastWasNewline = false;
        }
        // List items
        else if (tagName == "li") {
          if (!isClosing) {
            if (!lastWasNewline) out += "\n";
            out += "- ";
            lastWasNewline = false;
          } else {
            out += "\n";
            lastWasNewline = true;
          }
        }
        // Unordered/ordered list
        else if (tagName == "ul" || tagName == "ol") {
          if (isClosing) {
            if (!lastWasNewline) out += "\n";
            lastWasNewline = true;
          }
        }
        // Blockquote
        else if (tagName == "blockquote") {
          if (!isClosing) {
            if (!lastWasNewline) out += "\n\n";
            out += "> ";
            inBlockquote = true;
            lastWasNewline = false;
          } else {
            out += "\n\n";
            inBlockquote = false;
            lastWasNewline = true;
          }
        }
        // Line break
        else if (tagName == "br" || tagName == "br/") {
          out += "\n";
          if (inBlockquote) out += "> ";
          lastWasNewline = true;
        }
        // Links
        else if (tagName == "a") {
          if (!isClosing) {
            linkHref = extractHref(rawTag);
            linkText.clear();
            inLink = true;
          } else if (inLink) {
            if (!linkHref.empty()) {
              out += "[" + linkText + "](" + linkHref + ")";
            } else {
              out += linkText;
            }
            inLink = false;
            linkText.clear();
            linkHref.clear();
            lastWasNewline = false;
          }
        }
        // hr
        else if (tagName == "hr" || tagName == "hr/") {
          if (!lastWasNewline) out += "\n";
          out += "\n---\n\n";
          lastWasNewline = true;
        }
        // All other tags are stripped

        i++;
        continue;
      }
      tagContent += c;
      i++;
      continue;
    }

    // TEXT state
    if (c == '<') {
      state = TAG;
      tagContent.clear();
      i++;
      continue;
    }

    // Handle HTML entities
    if (c == '&') {
      size_t consumed;
      std::string decoded = decodeEntity(html, i, consumed);
      if (consumed > 0) {
        if (inLink) {
          linkText += decoded;
        } else {
          out += decoded;
          lastWasNewline = false;
        }
        i += consumed;
        continue;
      }
    }

    // Regular text
    if (inLink) {
      linkText += c;
    } else {
      // Collapse whitespace
      if (c == '\n' || c == '\r' || c == '\t') {
        c = ' ';
      }
      if (c == ' ' && lastWasNewline) {
        // Skip leading whitespace after newline
      } else {
        out += c;
        lastWasNewline = (c == '\n');
      }
    }

    i++;
  }

  // Trim trailing whitespace
  while (!out.empty() && (out.back() == ' ' || out.back() == '\n' || out.back() == '\r')) {
    out.pop_back();
  }
  out += "\n";

  return out;
}
