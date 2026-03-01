#include "HtmlReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Serialization.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "CrossPointSettings.h"
#include "util/LangDetect.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long goHomeMs = 1000;
constexpr int statusBarMargin = 25;
constexpr int progressBarMarginTop = 1;

struct ArticleMeta {
  std::string title;
  std::string source;
  long timestamp = 0;
};

static bool readArticleMeta(const std::string& htmlPath, ArticleMeta& out) {
  FsFile mf;
  if (!Storage.openFileForRead("HTML", htmlPath + ".meta", mf)) return false;
  char* buf = static_cast<char*>(malloc(640));
  if (!buf) { mf.close(); return false; }
  const int n = mf.read(buf, 639);
  mf.close();
  if (n <= 0) { free(buf); return false; }
  buf[n] = '\0';
  for (int k = n - 1; k >= 0 && (buf[k] == '\n' || buf[k] == '\r'); k--) buf[k] = '\0';
  const std::string line(buf);
  free(buf);
  // title may contain '|' — parse from the right
  const size_t p2 = line.rfind('|');
  const size_t p1 = (p2 != std::string::npos && p2 > 0) ? line.rfind('|', p2 - 1) : std::string::npos;
  out.title     = (p1 != std::string::npos) ? line.substr(0, p1) : line;
  out.source    = (p1 != std::string::npos && p2 != std::string::npos) ? line.substr(p1 + 1, p2 - p1 - 1) : "";
  const std::string tsStr = (p2 != std::string::npos) ? line.substr(p2 + 1) : "";
  out.timestamp = tsStr.empty() ? 0L : static_cast<long>(atol(tsStr.c_str()));
  return true;
}

static std::string xmlEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if      (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else               out += static_cast<char>(c);
  }
  return out;
}

static std::string formatTimestamp(long ts) {
  if (ts <= 0) return "";
  const time_t t = static_cast<time_t>(ts);
  struct tm* tm = localtime(&t);
  if (!tm) return "";
  char buf[32];
  strftime(buf, sizeof(buf), "%d %b %Y", tm);
  return buf;
}

// Section cache file format (mirrors MdReaderActivity)
constexpr uint8_t SECTION_FILE_VERSION = 1;
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(uint16_t) +
                                 sizeof(uint32_t);

static bool isVoidElement(const char* name, int len) {
  static const char* const kVoidTags[] = {
      "area", "base", "br",     "col",   "embed", "hr",    "img",
      "input", "link", "meta", "param", "source", "track", "wbr"};
  for (const auto* tag : kVoidTags) {
    if (len == static_cast<int>(strlen(tag)) && strncasecmp(name, tag, len) == 0) return true;
  }
  return false;
}

// Stream-converts HTML5 to XHTML: void elements become self-closing, and if injectLang is
// non-empty the <html> tag gets a lang="xx" attribute inserted (if not already present).
// Reads inPath in 512-byte chunks, writes to outPath. Returns true on success.
// bodyPreamble: injected right after <body...> for full HTML documents.
// prependContent / appendContent: written before/after the streamed HTML for bare fragments
// (use these together to wrap fragment + preamble in a single XML root element).
static bool sanitizeHtmlForExpat(const std::string& inPath, const std::string& outPath,
                                  const std::string& injectLang = "",
                                  const std::string& bodyPreamble = "",
                                  const std::string& prependContent = "",
                                  const std::string& appendContent = "") {
  enum State {
    S_NORMAL,
    S_TAG_OPEN,
    S_TAG_NAME,
    S_VOID_ATTRS,
    S_VOID_DQUOTE,
    S_VOID_SQUOTE,
    S_SKIP,
    S_SKIP_DQUOTE,
    S_SKIP_SQUOTE,
    S_BANG,
    S_COMMENT_MAYBE,
    S_IN_COMMENT,
    S_COMMENT_END1,
    S_COMMENT_END2,
    // Raw-text elements (<script>, <style>): opening tag emitted empty, body discarded
    S_RAWTEXT_TAG,    // inside opening tag attrs — discard, look for closing '>'
    S_RAWTEXT_BODY,   // discarding body, watching for '<'
    S_RAWTEXT_LT,     // saw '<' in body
    S_RAWTEXT_SLASH,  // saw '</' in body, matching end tag name
    S_RAWTEXT_ENDTAG, // collected end tag name, waiting for '>'
    // Close-tag name collection (to drop </void-element> close tags)
    S_CLOSE_TAG_NAME, // collecting chars after '</'
    S_CLOSE_VOID_SKIP,// confirmed void close tag, eating until '>'
  };

  FsFile in, out;
  if (!Storage.openFileForRead("SAN", inPath, in)) {
    LOG_ERR("SAN", "Cannot open input: %s", inPath.c_str());
    return false;
  }
  if (!Storage.openFileForWrite("SAN", outPath, out)) {
    LOG_ERR("SAN", "Cannot create output: %s", outPath.c_str());
    in.close();
    return false;
  }

  // Write prepend content immediately (used for fragment HTML with no <body> tag)
  if (!prependContent.empty()) {
    if (out.write(prependContent.c_str(), prependContent.size()) != prependContent.size()) {
      in.close();
      out.close();
      return false;
    }
  }

  constexpr int kBuf = 512;
  char* rbuf = static_cast<char*>(malloc(kBuf));
  char* wbuf = static_cast<char*>(malloc(kBuf));
  if (!rbuf || !wbuf) {
    free(rbuf);
    free(wbuf);
    in.close();
    out.close();
    return false;
  }

  State state = S_NORMAL;
  char tagNameBuf[24] = {};
  int tagNameLen = 0;
  char lastSig = 0;  // last non-whitespace char seen inside a void tag
  // html-tag lang injection tracking
  bool inHtmlTag = false;
  bool sawLangAttr = false;
  int langPatPos = 0;  // match position in "lang="
  bool inBodyTag = false;
  // raw-text element tracking (script/style)
  char rawTagName[8] = {};
  int rawTagLen = 0;
  int rawEndPos = 0;
  int wpos = 0;
  bool ok = true;

  auto flushOut = [&]() {
    if (wpos > 0) {
      if (out.write(wbuf, wpos) != static_cast<size_t>(wpos)) ok = false;
      wpos = 0;
    }
  };
  auto emit = [&](char c) {
    wbuf[wpos++] = c;
    if (wpos == kBuf) flushOut();
  };
  auto emitCStr = [&](const char* s) {
    while (*s) emit(*s++);
  };

  for (;;) {
    const int n = in.read(rbuf, kBuf);
    if (n <= 0) break;
    for (int i = 0; i < n && ok; i++) {
      const char c = rbuf[i];
      switch (state) {
        case S_NORMAL:
          if (c == '<') {
            // Defer emit until S_TAG_OPEN confirms it's not a void-element close tag
            state = S_TAG_OPEN;
          } else {
            emit(c);
          }
          break;

        case S_TAG_OPEN:
          if (c == '!') {
            emit('<'); emit(c);
            state = S_BANG;
          } else if (c == '?') {
            emit('<'); emit(c);
            state = S_SKIP;
          } else if (c == '/') {
            // Collect closing tag name before deciding whether to emit
            tagNameLen = 0;
            state = S_CLOSE_TAG_NAME;
          } else if (isalpha(static_cast<unsigned char>(c))) {
            emit('<');
            tagNameBuf[0] = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            tagNameLen = 1;
            emit(c);
            state = S_TAG_NAME;
          } else {
            emit('<'); emit(c);
            state = S_NORMAL;
          }
          break;

        case S_TAG_NAME:
          if (isalpha(static_cast<unsigned char>(c)) || isdigit(static_cast<unsigned char>(c)) || c == '-') {
            if (tagNameLen < 23) tagNameBuf[tagNameLen++] = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            emit(c);
          } else {
            tagNameBuf[tagNameLen] = '\0';
            if (isVoidElement(tagNameBuf, tagNameLen)) {
              lastSig = tagNameLen > 0 ? tagNameBuf[tagNameLen - 1] : 0;
              if (c == '>') {
                emit('/');
                emit('>');
                state = S_NORMAL;
              } else {
                emit(c);
                if (c == '/') lastSig = '/';
                else if (c != ' ' && c != '\t' && c != '\n' && c != '\r') lastSig = c;
                state = S_VOID_ATTRS;
              }
            } else {
              const bool isHtml = (tagNameLen == 4 && tagNameBuf[0] == 'h' && tagNameBuf[1] == 't' &&
                                   tagNameBuf[2] == 'm' && tagNameBuf[3] == 'l');
              const bool isBody = (tagNameLen == 4 && tagNameBuf[0] == 'b' && tagNameBuf[1] == 'o' &&
                                   tagNameBuf[2] == 'd' && tagNameBuf[3] == 'y');
              const bool isRawText = (tagNameLen == 6 && strcmp(tagNameBuf, "script") == 0) ||
                                     (tagNameLen == 5 && strcmp(tagNameBuf, "style") == 0);
              if (isRawText) {
                // Store end tag name for matching; emit opening tag self-closed, discard body
                memcpy(rawTagName, tagNameBuf, tagNameLen);
                rawTagName[tagNameLen] = '\0';
                rawTagLen = tagNameLen;
                rawEndPos = 0;
                if (c == '>') {
                  emit('>');
                  emitCStr("</");
                  emitCStr(rawTagName);
                  emit('>');
                  state = S_RAWTEXT_BODY;
                } else {
                  emit(c);
                  state = S_RAWTEXT_TAG;
                }
              } else if (c == '>') {
                if (isHtml && !injectLang.empty()) {
                  emitCStr(" lang=\"");
                  emitCStr(injectLang.c_str());
                  emit('"');
                }
                emit('>');
                if (isBody && !bodyPreamble.empty()) emitCStr(bodyPreamble.c_str());
                state = S_NORMAL;
              } else {
                emit(c);
                if (isHtml) {
                  inHtmlTag = true;
                  sawLangAttr = false;
                  langPatPos = 0;
                }
                if (isBody) inBodyTag = true;
                if (c == '"') state = S_SKIP_DQUOTE;
                else if (c == '\'') state = S_SKIP_SQUOTE;
                else state = S_SKIP;
              }
            }
          }
          break;

        case S_VOID_ATTRS:
          if (c == '>') {
            if (lastSig != '/') emit('/');
            emit('>');
            state = S_NORMAL;
          } else if (c == '"') {
            emit(c);
            lastSig = c;
            state = S_VOID_DQUOTE;
          } else if (c == '\'') {
            emit(c);
            lastSig = c;
            state = S_VOID_SQUOTE;
          } else {
            emit(c);
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') lastSig = c;
          }
          break;

        case S_VOID_DQUOTE:
          emit(c);
          if (c == '"') { lastSig = c; state = S_VOID_ATTRS; }
          break;

        case S_VOID_SQUOTE:
          emit(c);
          if (c == '\'') { lastSig = c; state = S_VOID_ATTRS; }
          break;

        case S_SKIP:
          if (inHtmlTag && !sawLangAttr) {
            // Track "lang=" pattern via simple KMP-style matcher
            static const char kLangPat[] = "lang=";
            const char lc = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (lc == kLangPat[langPatPos]) {
              if (++langPatPos == 5) { sawLangAttr = true; langPatPos = 0; }
            } else {
              langPatPos = (lc == kLangPat[0]) ? 1 : 0;
            }
          }
          if (c == '>') {
            if (inHtmlTag) {
              if (!sawLangAttr && !injectLang.empty()) {
                emitCStr(" lang=\"");
                emitCStr(injectLang.c_str());
                emit('"');
              }
              inHtmlTag = false;
            }
            emit(c);
            if (inBodyTag && !bodyPreamble.empty()) {
              emitCStr(bodyPreamble.c_str());
              inBodyTag = false;
            }
            state = S_NORMAL;
          } else {
            emit(c);
            if (c == '"') state = S_SKIP_DQUOTE;
            else if (c == '\'') state = S_SKIP_SQUOTE;
          }
          break;

        case S_SKIP_DQUOTE:
          emit(c);
          if (c == '"') state = S_SKIP;
          break;

        case S_SKIP_SQUOTE:
          emit(c);
          if (c == '\'') state = S_SKIP;
          break;

        case S_BANG:
          emit(c);
          state = (c == '-') ? S_COMMENT_MAYBE : S_SKIP;
          break;

        case S_COMMENT_MAYBE:
          emit(c);
          state = (c == '-') ? S_IN_COMMENT : S_SKIP;
          break;

        case S_IN_COMMENT:
          emit(c);
          if (c == '-') state = S_COMMENT_END1;
          break;

        case S_COMMENT_END1:
          emit(c);
          state = (c == '-') ? S_COMMENT_END2 : S_IN_COMMENT;
          break;

        case S_COMMENT_END2:
          emit(c);
          if (c == '>') state = S_NORMAL;
          else if (c != '-') state = S_IN_COMMENT;
          // c == '-': stay (multiple trailing dashes)
          break;

        case S_CLOSE_TAG_NAME:
          if (isalpha(static_cast<unsigned char>(c)) || isdigit(static_cast<unsigned char>(c)) || c == '-') {
            if (tagNameLen < 23) tagNameBuf[tagNameLen++] = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            // Don't emit yet — still collecting name
          } else {
            tagNameBuf[tagNameLen] = '\0';
            if (isVoidElement(tagNameBuf, tagNameLen)) {
              // Drop entire </void-element> close tag
              if (c == '>') state = S_NORMAL;
              else state = S_CLOSE_VOID_SKIP;
            } else {
              // Not a void element — emit the buffered '</' + name + current char
              emit('<'); emit('/');
              for (int k = 0; k < tagNameLen; k++) emit(tagNameBuf[k]);
              emit(c);
              if (c == '>') state = S_NORMAL;
              else if (c == '"') state = S_SKIP_DQUOTE;
              else if (c == '\'') state = S_SKIP_SQUOTE;
              else state = S_SKIP;
            }
          }
          break;

        case S_CLOSE_VOID_SKIP:
          if (c == '>') state = S_NORMAL;
          break;

        case S_RAWTEXT_TAG:
          // Inside <script .../> or <style .../> opening tag — emit attrs, look for '>'
          emit(c);
          if (c == '>') {
            emitCStr("</");
            emitCStr(rawTagName);
            emit('>');
            rawEndPos = 0;
            state = S_RAWTEXT_BODY;
          }
          // Ignore quote states inside raw-text opening tag attrs (safe: no body content yet)
          break;

        case S_RAWTEXT_BODY:
          // Discard everything; watch for '<' that might start the end tag
          if (c == '<') state = S_RAWTEXT_LT;
          break;

        case S_RAWTEXT_LT:
          if (c == '/') { rawEndPos = 0; state = S_RAWTEXT_SLASH; }
          else if (c != '<') state = S_RAWTEXT_BODY;
          // c == '<': another '<', stay in LT
          break;

        case S_RAWTEXT_SLASH:
          {
            const char lc = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (rawEndPos < rawTagLen && lc == rawTagName[rawEndPos]) {
              if (++rawEndPos == rawTagLen) state = S_RAWTEXT_ENDTAG;
            } else {
              state = S_RAWTEXT_BODY;
            }
          }
          break;

        case S_RAWTEXT_ENDTAG:
          // Waiting for '>' (or whitespace) after matched end tag name
          if (c == '>') state = S_NORMAL;
          else if (c != ' ' && c != '\t' && c != '\n' && c != '\r') state = S_RAWTEXT_BODY;
          // whitespace before '>': stay
          break;
      }
    }
  }

  if (ok) {
    flushOut();
    if (!appendContent.empty()) {
      if (out.write(appendContent.c_str(), appendContent.size()) != appendContent.size()) ok = false;
    }
  }
  free(rbuf);
  free(wbuf);
  in.close();
  out.close();
  return ok;
}

}  // namespace

void HtmlReaderActivity::taskTrampoline(void* param) {
  auto* self = static_cast<HtmlReaderActivity*>(param);
  self->displayTaskLoop();
}

void HtmlReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (!wa) {
    return;
  }

  switch (SETTINGS.orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }

  renderingMutex = xSemaphoreCreateMutex();

  if (!wa->setupCacheDir()) {
    LOG_ERR("HTML", "setupCacheDir failed in onEnter (will retry in createSectionCache)");
  }
  sectionFilePath = wa->getCachePath() + "/section.bin";

  auto filePath = wa->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, wa->getAuthor(), "");

  updateRequired = true;

  xTaskCreate(&HtmlReaderActivity::taskTrampoline, "HtmlReaderTask",
              8192, this, 1, &displayTaskHandle);
}

void HtmlReaderActivity::onExit() {
  ActivityWithSubactivity::onExit();

  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  pageLut.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  wa.reset();
}

void HtmlReaderActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  const bool usePressForPageTurn = !SETTINGS.longPressChapterSkip;
  const bool prevTriggered = usePressForPageTurn ? (mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
                                                    mappedInput.wasPressed(MappedInputManager::Button::Left))
                                                 : (mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                                                    mappedInput.wasReleased(MappedInputManager::Button::Left));
  const bool powerPageTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                             mappedInput.wasReleased(MappedInputManager::Button::Power);
  const bool nextTriggered = usePressForPageTurn
                                 ? (mappedInput.wasPressed(MappedInputManager::Button::PageForward) || powerPageTurn ||
                                    mappedInput.wasPressed(MappedInputManager::Button::Right))
                                 : (mappedInput.wasReleased(MappedInputManager::Button::PageForward) || powerPageTurn ||
                                    mappedInput.wasReleased(MappedInputManager::Button::Right));

  // End-action overlay: shown after reaching last page and pressing next once more.
  // Buttons are remapped: Back=List, Confirm=Delete, Prev=prev page, Next=next article.
  if (endActionsVisible) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs)) {
      endActionsVisible = false;
      onGoBack();
      return;
    }
    if (onDeleteAndAdvance && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      onDeleteAndAdvance();
      return;
    }
    if (prevTriggered && currentPage > 0) {
      currentPage--;
      endActionsVisible = false;
      updateRequired = true;
      return;
    }
    if (onAdvanceArticle && nextTriggered) {
      onAdvanceArticle();
      return;
    }
    return;  // eat all other input while overlay is shown
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
    onGoHome();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < goHomeMs) {
    onGoBack();
    return;
  }

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // On last page + next → show end-action overlay (if article callbacks present)
  const bool onLastPage = initialized && totalPages > 0 && currentPage == totalPages - 1;
  if (onLastPage && (onAdvanceArticle || onDeleteAndAdvance) && nextTriggered) {
    endActionsVisible = true;
    updateRequired = true;
    return;
  }

  if (prevTriggered && currentPage > 0) {
    currentPage--;
    updateRequired = true;
  } else if (nextTriggered && currentPage < totalPages - 1) {
    currentPage++;
    updateRequired = true;
  }
}

void HtmlReaderActivity::displayTaskLoop() {
  if (!initialized) {
    initializeReader();
    updateRequired = true;
  }

  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void HtmlReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += cachedScreenMargin;
  orientedMarginLeft += cachedScreenMargin;
  orientedMarginRight += cachedScreenMargin;
  orientedMarginBottom += cachedScreenMargin;

  auto metrics = UITheme::getInstance().getMetrics();

  if (SETTINGS.statusBar != CrossPointSettings::STATUS_BAR_MODE::NONE) {
    const bool showProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
    orientedMarginBottom += statusBarMargin - cachedScreenMargin +
                            (showProgressBar ? (metrics.progressBarHeight + progressBarMarginTop) : 0);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  const float lineCompression = SETTINGS.getReaderLineCompression();
  const bool extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
  const bool hyphenationEnabled = SETTINGS.hyphenationEnabled;

  if (!loadSectionCache(cachedFontId, lineCompression, extraParagraphSpacing, cachedParagraphAlignment, viewportWidth,
                        viewportHeight, hyphenationEnabled)) {
    LOG_DBG("HTML", "Cache not found, building...");
    if (!createSectionCache(cachedFontId, lineCompression, extraParagraphSpacing, cachedParagraphAlignment,
                            viewportWidth, viewportHeight, hyphenationEnabled)) {
      LOG_ERR("HTML", "Failed to build section cache");
      initialized = true;
      return;
    }
  } else {
    LOG_DBG("HTML", "Cache found, %d pages", totalPages);
  }

  loadProgress();
  initialized = true;
}

bool HtmlReaderActivity::loadSectionCache(const int fontId, const float lineCompression,
                                          const bool extraParagraphSpacing, const uint8_t paragraphAlignment,
                                          const uint16_t viewportWidth, const uint16_t viewportHeight,
                                          const bool hyphenationEnabled) {
  FsFile file;
  if (!Storage.openFileForRead("HTML", sectionFilePath, file)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(file, version);
  if (version != SECTION_FILE_VERSION) {
    file.close();
    LOG_DBG("HTML", "Cache version mismatch (%d != %d)", version, SECTION_FILE_VERSION);
    return false;
  }

  int fileFontId;
  float fileLineCompression;
  bool fileExtraParagraphSpacing;
  uint8_t fileParagraphAlignment;
  uint16_t fileViewportWidth, fileViewportHeight;
  bool fileHyphenationEnabled;
  serialization::readPod(file, fileFontId);
  serialization::readPod(file, fileLineCompression);
  serialization::readPod(file, fileExtraParagraphSpacing);
  serialization::readPod(file, fileParagraphAlignment);
  serialization::readPod(file, fileViewportWidth);
  serialization::readPod(file, fileViewportHeight);
  serialization::readPod(file, fileHyphenationEnabled);

  if (fontId != fileFontId || lineCompression != fileLineCompression ||
      extraParagraphSpacing != fileExtraParagraphSpacing || paragraphAlignment != fileParagraphAlignment ||
      viewportWidth != fileViewportWidth || viewportHeight != fileViewportHeight ||
      hyphenationEnabled != fileHyphenationEnabled) {
    file.close();
    LOG_DBG("HTML", "Cache parameters mismatch, rebuilding");
    Storage.remove(sectionFilePath.c_str());
    return false;
  }

  uint16_t pageCount;
  serialization::readPod(file, pageCount);
  totalPages = pageCount;

  uint32_t lutOffset;
  serialization::readPod(file, lutOffset);

  pageLut.clear();
  pageLut.reserve(totalPages);
  file.seek(lutOffset);
  for (int i = 0; i < totalPages; i++) {
    uint32_t pos;
    serialization::readPod(file, pos);
    pageLut.push_back(pos);
  }

  file.close();
  return true;
}

bool HtmlReaderActivity::createSectionCache(const int fontId, const float lineCompression,
                                            const bool extraParagraphSpacing, const uint8_t paragraphAlignment,
                                            const uint16_t viewportWidth, const uint16_t viewportHeight,
                                            const bool hyphenationEnabled) {
  // Retry dir creation in case onEnter's attempt failed (e.g. SD not ready yet)
  wa->setupCacheDir();

  FsFile file;
  if (!Storage.openFileForWrite("HTML", sectionFilePath, file)) {
    LOG_ERR("HTML", "Failed to open section file for writing");
    return false;
  }

  serialization::writePod(file, SECTION_FILE_VERSION);
  serialization::writePod(file, fontId);
  serialization::writePod(file, lineCompression);
  serialization::writePod(file, extraParagraphSpacing);
  serialization::writePod(file, paragraphAlignment);
  serialization::writePod(file, viewportWidth);
  serialization::writePod(file, viewportHeight);
  serialization::writePod(file, hyphenationEnabled);
  serialization::writePod(file, static_cast<uint16_t>(0));  // placeholder page count
  serialization::writePod(file, static_cast<uint32_t>(0));  // placeholder LUT offset

  pageLut.clear();
  uint16_t pageCount = 0;

  const std::string htmlPath = wa->getPath();

  std::string lang = detectArticleLanguage(htmlPath);
  if (lang.empty()) lang = "en";
  LOG_INF("HTML", "Article language: %s", lang.c_str());

  if (hyphenationEnabled) {
    Hyphenator::setPreferredLanguage(lang);
  }

  // Build article header preamble from sidecar .meta file
  std::string bodyPreamble;
  {
    ArticleMeta meta;
    if (readArticleMeta(htmlPath, meta)) {
      std::string metaLine = xmlEscape(meta.source);
      const std::string date = formatTimestamp(meta.timestamp);
      if (!date.empty()) metaLine += (metaLine.empty() ? "" : " \xC2\xB7 ") + date;
      bodyPreamble = "<div class=\"cp-header\"><br/><h1>" + xmlEscape(meta.title) + "</h1>"
                   + (metaLine.empty() ? "" : "<p>" + metaLine + "</p>")
                   + "</div>";
    }
  }

  // Detect whether the HTML is a full document (has <body>) or a bare fragment
  bool hasBodyTag = false;
  {
    FsFile probe;
    if (Storage.openFileForRead("HTML", htmlPath, probe)) {
      char* scan = static_cast<char*>(malloc(2048));
      if (scan) {
        const int n = probe.read(scan, 2047);
        probe.close();
        if (n > 0) {
          scan[n] = '\0';
          for (int k = 0; k < n - 4; k++) {
            if (scan[k] == '<' && (scan[k+1] == 'b' || scan[k+1] == 'B') &&
                (scan[k+2] == 'o' || scan[k+2] == 'O') && (scan[k+3] == 'd' || scan[k+3] == 'D') &&
                (scan[k+4] == 'y' || scan[k+4] == 'Y')) {
              hasBodyTag = true;
              break;
            }
          }
        }
        free(scan);
      } else {
        probe.close();
      }
    }
  }

  const std::string sanitizedPath = wa->getCachePath() + "/sanitized.html";
  const bool didSanitize = hasBodyTag
      ? sanitizeHtmlForExpat(htmlPath, sanitizedPath, lang, bodyPreamble)
      : sanitizeHtmlForExpat(htmlPath, sanitizedPath, lang, "",
                             "<div class=\"cp-root\">" + bodyPreamble, "</div>");
  if (!didSanitize) {
    LOG_ERR("HTML", "Sanitize failed, attempting parse on original");
  }
  const std::string& parserInputPath = didSanitize ? sanitizedPath : htmlPath;

  ChapterHtmlSlimParser parser(
      nullptr,   // epub = null — no EPUB context, images skipped
      parserInputPath, renderer, fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
      viewportHeight, hyphenationEnabled,
      [&file, &pageCount, this](std::unique_ptr<Page> page) {
        const uint32_t position = file.position();
        if (page->serialize(file)) {
          pageLut.push_back(position);
          pageCount++;
          LOG_DBG("HTML", "Page %d processed", pageCount);
        }
      },
      false,  // embeddedStyle — no external CSS for web articles
      "",     // contentBase — images skipped (epub=null)
      "",     // imageBasePath — images skipped
      [this]() { GUI.drawPopup(renderer, "Indexing..."); });

  const bool parseOk = parser.parseAndBuildPages();
  if (didSanitize) {
    Storage.remove(sanitizedPath.c_str());
  }
  if (!parseOk) {
    LOG_ERR("HTML", "Failed to parse HTML");
    file.close();
    Storage.remove(sectionFilePath.c_str());
    return false;
  }

  const uint32_t lutOffset = file.position();
  for (const uint32_t& pos : pageLut) {
    serialization::writePod(file, pos);
  }

  file.seek(HEADER_SIZE - sizeof(uint32_t) - sizeof(uint16_t));
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  file.close();

  totalPages = pageCount;
  LOG_DBG("HTML", "Built section cache: %d pages", totalPages);
  return true;
}

std::unique_ptr<Page> HtmlReaderActivity::loadPageFromCache(int pageIndex) {
  if (pageIndex < 0 || pageIndex >= static_cast<int>(pageLut.size())) {
    return nullptr;
  }

  FsFile file;
  if (!Storage.openFileForRead("HTML", sectionFilePath, file)) {
    return nullptr;
  }

  file.seek(pageLut[pageIndex]);
  auto page = Page::deserialize(file);
  file.close();
  return page;
}

void HtmlReaderActivity::renderScreen() {
  if (!wa) {
    return;
  }

  if (totalPages == 0) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += cachedScreenMargin;
  orientedMarginLeft += cachedScreenMargin;
  orientedMarginRight += cachedScreenMargin;
  orientedMarginBottom += cachedScreenMargin;

  auto metrics = UITheme::getInstance().getMetrics();
  if (SETTINGS.statusBar != CrossPointSettings::STATUS_BAR_MODE::NONE) {
    const bool showProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
    orientedMarginBottom += statusBarMargin - cachedScreenMargin +
                            (showProgressBar ? (metrics.progressBarHeight + progressBarMarginTop) : 0);
  }

  auto page = loadPageFromCache(currentPage);
  if (!page) {
    LOG_ERR("HTML", "Failed to load page %d from cache", currentPage);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  renderer.clearScreen();
  renderContents(std::move(page), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
  saveProgress();
}

void HtmlReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  page->render(renderer, cachedFontId, orientedMarginLeft, orientedMarginTop);
  renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);

  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  if (SETTINGS.textAntiAliasing && renderer.storeBwBuffer()) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer, cachedFontId, orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleLsbBuffers();

    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer, cachedFontId, orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);

    renderer.restoreBwBuffer();
  }
}

void HtmlReaderActivity::renderStatusBar(const int /*orientedMarginRight*/, const int /*orientedMarginBottom*/,
                                         const int /*orientedMarginLeft*/) {
  if (endActionsVisible) {
    const char* prevLabel    = currentPage > 0       ? tr(STR_PREV_PAGE)    : "";
    const char* nextLabel    = onAdvanceArticle       ? tr(STR_NEXT_ARTICLE) : "";
    const char* deleteLabel  = onDeleteAndAdvance     ? tr(STR_DELETE)       : "";
    const auto labels = mappedInput.mapLabels(tr(STR_ARTICLE_LIST), deleteLabel, prevLabel, nextLabel);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;
  const std::string title = wa->getTitle();
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, title);
}

void HtmlReaderActivity::saveProgress() const {
  FsFile f;
  if (Storage.openFileForWrite("HTML", wa->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = 0;
    data[3] = 0;
    f.write(data, 4);
    f.close();
  }
}

void HtmlReaderActivity::loadProgress() {
  FsFile f;
  if (Storage.openFileForRead("HTML", wa->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] + (data[1] << 8);
      if (currentPage >= totalPages) currentPage = totalPages - 1;
      if (currentPage < 0) currentPage = 0;
      LOG_DBG("HTML", "Loaded progress: page %d/%d", currentPage, totalPages);
    }
    f.close();
  }
}
