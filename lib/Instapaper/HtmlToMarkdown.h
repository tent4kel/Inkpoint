#pragma once
#include <string>

namespace HtmlToMarkdown {

// Convert HTML to Markdown. Designed for Instapaper's clean article HTML.
std::string convert(const std::string& html);

}  // namespace HtmlToMarkdown
