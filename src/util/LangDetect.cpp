#include "LangDetect.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cctype>
#include <cstring>

namespace {

// All supported primary language tags (en included so explicit lang="en" is honoured).
static const char* const kSupportedLangs[] = {"nl", "de", "fr", "it", "es", "pl", "en"};
constexpr int kSupportedCount = 7;

// Step 1: scan first 512 bytes for a lang="xx" or lang='xx' attribute.
// Returns lowercase primary subtag if it is in the supported set, else "".
static std::string langFromAttr(const std::string& htmlPath) {
  constexpr int kScanBytes = 512;
  char* buf = static_cast<char*>(malloc(kScanBytes + 1));
  if (!buf) return "";

  FsFile f;
  if (!Storage.openFileForRead("LANG", htmlPath, f)) {
    free(buf);
    return "";
  }
  const int n = f.read(buf, kScanBytes);
  f.close();
  if (n < 6) { free(buf); return ""; }
  buf[n] = '\0';

  for (int i = 0; i <= n - 6; i++) {
    // Case-insensitive match for "lang="
    if (tolower(static_cast<unsigned char>(buf[i]))     != 'l') continue;
    if (tolower(static_cast<unsigned char>(buf[i + 1])) != 'a') continue;
    if (tolower(static_cast<unsigned char>(buf[i + 2])) != 'n') continue;
    if (tolower(static_cast<unsigned char>(buf[i + 3])) != 'g') continue;
    if (buf[i + 4] != '=') continue;

    int j = i + 5;
    // Allow optional whitespace between '=' and quote (rare but valid)
    while (j < n && (buf[j] == ' ' || buf[j] == '\t')) j++;
    if (j >= n) break;
    const char quote = buf[j];
    if (quote != '"' && quote != '\'') continue;
    j++;

    // Extract value, lowercase, max 10 chars
    char val[11];
    int vlen = 0;
    while (j < n && buf[j] != quote && vlen < 10) {
      val[vlen++] = static_cast<char>(tolower(static_cast<unsigned char>(buf[j++])));
    }
    if (vlen == 0) continue;

    // Take only the primary subtag (strip region/script: "de-AT" → "de")
    char primary[11];
    int plen = 0;
    while (plen < vlen && val[plen] != '-') {
      primary[plen] = val[plen];
      plen++;
    }
    primary[plen] = '\0';

    for (int k = 0; k < kSupportedCount; k++) {
      if (strcmp(primary, kSupportedLangs[k]) == 0) {
        free(buf);
        LOG_DBG("LANG", "Detected '%s' from lang attribute", primary);
        return primary;
      }
    }
  }

  free(buf);
  return "";
}

// Step 2: word-frequency scan of first 6 KB.
// "en" is not listed — it is the caller's fallback for an empty return.
static const struct {
  const char* lang;
  const char* words[5];
} kLangWords[] = {
    {"de", {"der",  "und",  "ist",  "nicht", "wird"}},
    {"nl", {"het",  "een",  "van",  "zijn",  "maar"}},
    {"fr", {"les",  "des",  "dans", "nous",  "vous"}},
    {"es", {"los",  "las",  "por",  "del",   "como"}},
    {"it", {"che",  "non",  "per",  "una",   "nel" }},
    {"pl", {"się",  "nie",  "jest", "jak",   "ale" }},
};
constexpr int kLangCount = static_cast<int>(sizeof(kLangWords) / sizeof(kLangWords[0]));

static std::string langFromWords(const std::string& htmlPath) {
  constexpr int kScanBytes = 6144;
  char* buf = static_cast<char*>(malloc(kScanBytes + 1));
  if (!buf) return "";

  FsFile f;
  if (!Storage.openFileForRead("LANG", htmlPath, f)) {
    free(buf);
    return "";
  }
  const int n = f.read(buf, kScanBytes);
  f.close();
  if (n <= 0) { free(buf); return ""; }
  buf[n] = '\0';

  int scores[kLangCount] = {};

  bool inTag = false;
  char inTagQuote = 0;
  char word[32];
  int wlen = 0;

  auto checkWord = [&]() {
    if (wlen == 0) return;
    word[wlen] = '\0';
    // Lowercase ASCII part; non-ASCII UTF-8 bytes pass through unchanged
    for (int i = 0; i < wlen; i++) {
      if (word[i] >= 'A' && word[i] <= 'Z') word[i] += 32;
    }
    for (int li = 0; li < kLangCount; li++) {
      for (int wi = 0; wi < 5; wi++) {
        if (strcmp(word, kLangWords[li].words[wi]) == 0) {
          scores[li]++;
          break;
        }
      }
    }
    wlen = 0;
  };

  for (int i = 0; i < n; i++) {
    const uint8_t c = static_cast<uint8_t>(buf[i]);
    if (inTag) {
      if (inTagQuote) {
        if (c == static_cast<uint8_t>(inTagQuote)) inTagQuote = 0;
      } else if (c == '"' || c == '\'') {
        inTagQuote = static_cast<char>(c);
      } else if (c == '>') {
        inTag = false;
      }
      continue;
    }
    if (c == '<') {
      checkWord();
      inTag = true;
      inTagQuote = 0;
      continue;
    }
    // Word chars: ASCII alpha or high UTF-8 byte (covers accented chars)
    const bool isWordChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80;
    if (isWordChar) {
      if (wlen < 31) word[wlen++] = static_cast<char>(c);
    } else {
      checkWord();
    }
  }
  checkWord();
  free(buf);

  int best = -1, bestScore = 0;
  for (int i = 0; i < kLangCount; i++) {
    if (scores[i] > bestScore) { bestScore = scores[i]; best = i; }
  }

  if (best < 0 || bestScore < 2) return "";
  LOG_DBG("LANG", "Detected '%s' (score %d) from word scan", kLangWords[best].lang, bestScore);
  return kLangWords[best].lang;
}

}  // namespace

std::string detectArticleLanguage(const std::string& htmlPath) {
  const std::string lang = langFromAttr(htmlPath);
  if (!lang.empty()) return lang;
  return langFromWords(htmlPath);
}
