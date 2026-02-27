#pragma once
#include <string>

// Returns a BCP-47 primary language tag ("de", "nl", "fr", "it", "es", "pl")
// or empty string if unknown (caller should default to "en").
// Step 1: looks for a lang="xx" attribute in the first 512 bytes of the file.
// Step 2: word-frequency scan of the first 6 KB (skipping tag content).
std::string detectArticleLanguage(const std::string& htmlPath);
