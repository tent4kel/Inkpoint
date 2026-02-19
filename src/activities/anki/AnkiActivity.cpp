#include "AnkiActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MarkdownParser.h>
#include <Serialization.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "anki/AnkiSessionManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int LABEL_HEIGHT = 30;
constexpr const char* TEMP_MD_PATH = "/.ankix/_card.md";
constexpr const char* ANKI_SETTINGS_PATH = "/.ankix/anki_settings.bin";
constexpr uint8_t ANKI_SETTINGS_VERSION = 2;
constexpr uint8_t DECK_SETTINGS_VERSION = 1;
constexpr unsigned long LONG_PRESS_MS = 800;

// Convert "/anki/Deck.csv" → "/.ankix/Deck.settings"
std::string deckSettingsPath(const std::string& csvPath) {
  size_t lastSlash = csvPath.find_last_of('/');
  std::string filename = (lastSlash != std::string::npos) ? csvPath.substr(lastSlash + 1) : csvPath;
  if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".csv") {
    filename = filename.substr(0, filename.size() - 4);
  }
  return "/.ankix/" + filename + ".settings";
}
}  // namespace

// --- Anki-specific settings (independent of reader) ---

int AnkiActivity::getFontIdForAnkiSize() const {
  // Use the global font family setting but our own size
  switch (SETTINGS.fontFamily) {
    case CrossPointSettings::BOOKERLY:
    default:
      switch (ankiFontSize) {
        case 0: return BOOKERLY_12_FONT_ID;
        case 1: default: return BOOKERLY_14_FONT_ID;
        case 2: return BOOKERLY_16_FONT_ID;
        case 3: return BOOKERLY_18_FONT_ID;
      }
    case CrossPointSettings::NOTOSANS:
      switch (ankiFontSize) {
        case 0: return NOTOSANS_12_FONT_ID;
        case 1: default: return NOTOSANS_14_FONT_ID;
        case 2: return NOTOSANS_16_FONT_ID;
        case 3: return NOTOSANS_18_FONT_ID;
      }
    case CrossPointSettings::OPENDYSLEXIC:
      switch (ankiFontSize) {
        case 0: return OPENDYSLEXIC_8_FONT_ID;
        case 1: default: return OPENDYSLEXIC_10_FONT_ID;
        case 2: return OPENDYSLEXIC_12_FONT_ID;
        case 3: return OPENDYSLEXIC_14_FONT_ID;
      }
  }
}

void AnkiActivity::loadAnkiSettings() {
  // Global settings: orientation (portrait/landscape)
  {
    FsFile f;
    if (Storage.openFileForRead("ANK", ANKI_SETTINGS_PATH, f)) {
      uint8_t version;
      serialization::readPod(f, version);
      if (version == ANKI_SETTINGS_VERSION) {
        serialization::readPod(f, ankiFontSize);  // legacy global font size (fallback)
        uint8_t portrait;
        serialization::readPod(f, portrait);
        ankiPortrait = portrait != 0;
        uint8_t swap;
        serialization::readPod(f, swap);
        ankiSwapFrontBack = swap != 0;  // legacy global swap (fallback)
      }
      f.close();
    }
  }

  // Per-deck settings: font size + swap (overrides global if file exists)
  {
    std::string path = deckSettingsPath(csvPath);
    FsFile f;
    if (Storage.openFileForRead("ANK", path.c_str(), f)) {
      uint8_t version;
      serialization::readPod(f, version);
      if (version == DECK_SETTINGS_VERSION) {
        serialization::readPod(f, ankiFontSize);
        uint8_t swap;
        serialization::readPod(f, swap);
        ankiSwapFrontBack = swap != 0;
      }
      f.close();
    }
  }

  if (ankiFontSize > 3) ankiFontSize = 1;
}

void AnkiActivity::saveAnkiSettings() {
  // Global settings: orientation only
  {
    FsFile f;
    if (Storage.openFileForWrite("ANK", ANKI_SETTINGS_PATH, f)) {
      serialization::writePod(f, ANKI_SETTINGS_VERSION);
      serialization::writePod(f, ankiFontSize);
      uint8_t portrait = ankiPortrait ? 1 : 0;
      serialization::writePod(f, portrait);
      uint8_t swap = ankiSwapFrontBack ? 1 : 0;
      serialization::writePod(f, swap);
      f.close();
    }
  }

  // Per-deck settings: font size + swap
  {
    std::string path = deckSettingsPath(csvPath);
    FsFile f;
    if (Storage.openFileForWrite("ANK", path.c_str(), f)) {
      serialization::writePod(f, DECK_SETTINGS_VERSION);
      serialization::writePod(f, ankiFontSize);
      uint8_t swap = ankiSwapFrontBack ? 1 : 0;
      serialization::writePod(f, swap);
      f.close();
    }
  }
}

void AnkiActivity::applyOrientation() {
  if (ankiPortrait) {
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  } else {
    renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
  }
}

void AnkiActivity::cycleFontSize() {
  ankiFontSize = (ankiFontSize + 1) % 4;
  cachedFontId = getFontIdForAnkiSize();
  saveAnkiSettings();
  LOG_DBG("ANK", "Font size changed to %d", ankiFontSize);
}

void AnkiActivity::toggleOrientation() {
  ankiPortrait = !ankiPortrait;
  applyOrientation();
  saveAnkiSettings();
  LOG_DBG("ANK", "Orientation toggled to %s", ankiPortrait ? "portrait" : "landscape");
}

AnkiActivity::CardMargins AnkiActivity::getCardMargins() const {
  int mTop, mRight, mBottom, mLeft;
  renderer.getOrientedViewableTRBL(&mTop, &mRight, &mBottom, &mLeft);

  auto metrics = UITheme::getInstance().getMetrics();

  // Label bar is always at logical top
  mTop += cachedScreenMargin + LABEL_HEIGHT;
  mLeft += cachedScreenMargin;
  mRight += cachedScreenMargin;
  mBottom += cachedScreenMargin;

  // Button hints are drawn at the physical bottom of the panel (portrait coords).
  // In portrait, physical bottom = logical bottom — no side hints, full width used.
  // In LandscapeCCW, physical bottom = logical right.
  if (ankiPortrait) {
    mBottom += metrics.buttonHintsHeight;
  } else {
    mRight += metrics.buttonHintsHeight;
  }

  return {mTop, mRight, mBottom, mLeft};
}

// --- Activity lifecycle ---

void AnkiActivity::taskTrampoline(void* param) {
  auto* self = static_cast<AnkiActivity*>(param);
  self->displayTaskLoop();
}

void AnkiActivity::onEnter() {
  Activity::onEnter();

  if (!Storage.exists("/.ankix")) {
    Storage.mkdir("/.ankix");
  }

  loadAnkiSettings();
  applyOrientation();
  cachedFontId = getFontIdForAnkiSize();
  cachedScreenMargin = SETTINGS.screenMargin;

  renderingMutex = xSemaphoreCreateMutex();

  deck = std::unique_ptr<AnkiDeck>(new AnkiDeck(csvPath));
  if (!deck->load()) {
    LOG_ERR("ANK", "Failed to load deck: %s", csvPath.c_str());
  }
  deck->buildDueList();
  reviewCompleted = false;

  // Save state for boot resume
  APP_STATE.openEpubPath = csvPath;
  APP_STATE.readerActivityLoadCount++;
  APP_STATE.saveToFile();

  state = State::DECK_SUMMARY;
  updateRequired = true;

  xTaskCreate(&AnkiActivity::taskTrampoline, "AnkiActivityTask", 8192, this, 1, &displayTaskHandle);
}

void AnkiActivity::onExit() {
  Activity::onExit();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  // Clear boot resume state
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  cardPages.clear();
  deck.reset();

  Storage.remove(TEMP_MD_PATH);
}

void AnkiActivity::displayTaskLoop() {
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

// --- Input handling ---

void AnkiActivity::loop() {
  // Lower rocker (Down): long press = toggle orientation, short press = cycle font size
  // Applies in all card review states
  if (state == State::FRONT || state == State::BACK) {
    // Long press: toggle orientation (fires while still held)
    if (mappedInput.isPressed(MappedInputManager::Button::Down) &&
        mappedInput.getHeldTime() >= LONG_PRESS_MS && !longPressHandled) {
      longPressHandled = true;
      toggleOrientation();
      if (deck->currentCard()) {
        buildCardPages(state == State::FRONT ? frontContent() : backContent());
        currentCardPage = 0;
      }
      updateRequired = true;
      return;
    }
    // Short press: cycle font size (fires on release)
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      if (!longPressHandled) {
        cycleFontSize();
        if (deck->currentCard()) {
          buildCardPages(state == State::FRONT ? frontContent() : backContent());
          currentCardPage = 0;
        }
        updateRequired = true;
      }
      longPressHandled = false;
      return;
    }
  }

  switch (state) {
    case State::DECK_SUMMARY: {
      // Back button or upper rocker exits
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
          mappedInput.wasReleased(MappedInputManager::Button::Up)) {
        onGoBack();
        return;
      }
      // Left button toggles swap front/back
      if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
        ankiSwapFrontBack = !ankiSwapFrontBack;
        saveAnkiSettings();
        updateRequired = true;
        break;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        if (deck) {
          deck->buildDueList();
          if (!deck->currentCard()) {
            // No due cards — study ahead with future cards
            deck->buildStudyAheadList();
          }
          if (deck->currentCard()) {
            state = State::FRONT;
            buildCardPages(frontContent());
            currentCardPage = 0;
          }
          // If still no cards (empty deck), stay on DECK_SUMMARY
          updateRequired = true;
        }
      }
      break;
    }

    case State::FRONT: {
      // Upper rocker = back to deck summary (exit review)
      if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
        deck->buildDueList();  // Refresh due count for summary
        reviewCompleted = true;
        state = State::DECK_SUMMARY;
        cardPages.clear();
        updateRequired = true;
        break;
      }

      // Any front button flips the card
      if (mappedInput.getPressedFrontButton() >= 0) {
        state = State::BACK;
        buildCardPages(backContent());
        currentCardPage = 0;
        updateRequired = true;
      }
      break;
    }

    case State::BACK: {
      // Upper rocker = back to front side
      if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
        state = State::FRONT;
        buildCardPages(frontContent());
        currentCardPage = 0;
        updateRequired = true;
        break;
      }

      // Grade with front buttons (raw index 0-3 = left to right)
      int btn = mappedInput.getPressedFrontButton();
      if (btn >= 0 && btn <= 3) {
        Grade grade = static_cast<Grade>(btn);
        bool more = deck->gradeCurrentCard(grade);

        if (more && deck->currentCard()) {
          state = State::FRONT;
          buildCardPages(frontContent());
          currentCardPage = 0;
        } else {
          // Round complete — rebuild due list and return to summary
          deck->buildDueList();
          reviewCompleted = true;
          state = State::DECK_SUMMARY;
          cardPages.clear();
        }
        updateRequired = true;
      }
      break;
    }
  }
}

// --- Card page building ---

void AnkiActivity::buildCardPages(const std::string& mdText) {
  cardPages.clear();
  cardContentHeight = 0;

  // Write text to temp file for MarkdownParser
  {
    FsFile f;
    if (!Storage.openFileForWrite("ANK", TEMP_MD_PATH, f)) {
      LOG_ERR("ANK", "Failed to write temp md file");
      return;
    }
    f.write(reinterpret_cast<const uint8_t*>(mdText.c_str()), mdText.size());
    f.close();
  }

  Markdown md(TEMP_MD_PATH, "/.ankix");
  if (!md.load()) {
    LOG_ERR("ANK", "Failed to load temp md");
    return;
  }

  // Compute viewport (content area between top bar and button hints)
  const auto m = getCardMargins();
  const uint16_t vpWidth = renderer.getScreenWidth() - m.left - m.right;
  const uint16_t vpHeight = renderer.getScreenHeight() - m.top - m.bottom;

  // Track content height for vertical centering
  int totalContentHeight = 0;

  MarkdownParser parser(
      md, renderer, cachedFontId,
      1.0f,   // lineCompression
      false,  // extraParagraphSpacing
      static_cast<uint8_t>(CrossPointSettings::CENTER_ALIGN),
      vpWidth, vpHeight,
      false,  // hyphenation
      [this, &totalContentHeight](std::unique_ptr<Page> page) {
        // Compute content height from page elements
        for (const auto& elem : page->elements) {
          int bottom = elem->yPos + renderer.getLineHeight(cachedFontId);
          if (bottom > totalContentHeight) totalContentHeight = bottom;
        }
        cardPages.push_back(std::move(page));
      });

  parser.parseAndBuildPages();
  cardContentHeight = totalContentHeight;
  LOG_DBG("ANK", "Built %zu pages, content height %d", cardPages.size(), cardContentHeight);
}

// --- Rendering ---

void AnkiActivity::renderScreen() {
  renderer.clearScreen();

  switch (state) {
    case State::DECK_SUMMARY:
      renderDeckSummary();
      break;
    case State::FRONT:
      renderCardSide("FRONT");
      break;
    case State::BACK:
      renderCardSide("BACK");
      break;
  }

  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  // Anti-aliasing pass for card views
  if (SETTINGS.textAntiAliasing && (state == State::FRONT || state == State::BACK)) {
    if (!cardPages.empty() && currentCardPage < static_cast<int>(cardPages.size())) {
      const auto m = getCardMargins();
      const int vpHeight = renderer.getScreenHeight() - m.top - m.bottom;
      const int yOffset = (cardPages.size() == 1 && cardContentHeight < vpHeight)
                              ? m.top + (vpHeight - cardContentHeight) / 2
                              : m.top;

      renderer.storeBwBuffer();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      cardPages[currentCardPage]->render(renderer, cachedFontId, m.left, yOffset);
      renderer.copyGrayscaleLsbBuffers();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      cardPages[currentCardPage]->render(renderer, cachedFontId, m.left, yOffset);
      renderer.copyGrayscaleMsbBuffers();

      renderer.displayGrayBuffer();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
    }
  }
}

void AnkiActivity::renderDeckSummary() {
  const int screenH = renderer.getScreenHeight();
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);

  if (!deck) {
    renderer.drawCenteredText(UI_12_FONT_ID, screenH / 2, "Failed to load deck", true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels("Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }

  const size_t dueCount = deck->getDueCount();

  // Vertically center the content block
  const int numLines = 6 + (reviewCompleted && dueCount == 0 ? 1 : 0);
  const int blockHeight = lineH * numLines + 8 * 4 + lineH * 2;
  int y = (screenH - blockHeight) / 2;

  // Deck title
  renderer.drawCenteredText(cachedFontId, y, deck->getTitle().c_str(), true, EpdFontFamily::BOLD);
  y += lineH * 2;

  // Stats
  char buf[64];
  snprintf(buf, sizeof(buf), "Total cards: %zu", deck->getTotalCards());
  renderer.drawCenteredText(UI_12_FONT_ID, y, buf);
  y += lineH + 8;

  snprintf(buf, sizeof(buf), "Session: %u", ANKI_SESSION.getSession());
  renderer.drawCenteredText(UI_12_FONT_ID, y, buf);
  y += lineH + 8;

  snprintf(buf, sizeof(buf), "Reviewed: %u/%u", ANKI_SESSION.getCardsReviewed(), SETTINGS.getDailyGoalValue());
  renderer.drawCenteredText(UI_12_FONT_ID, y, buf);
  y += lineH + 8;

  snprintf(buf, sizeof(buf), "Showing: %s first", ankiSwapFrontBack ? "Back" : "Front");
  renderer.drawCenteredText(UI_12_FONT_ID, y, buf);
  y += lineH + 8;

  snprintf(buf, sizeof(buf), "Due: %zu", dueCount);
  renderer.drawCenteredText(UI_12_FONT_ID, y, buf);
  y += lineH * 2;

  if (reviewCompleted && dueCount == 0) {
    renderer.drawCenteredText(UI_12_FONT_ID, y, "Wow, you made it! No cards due!", true);
  }

  const char* startLabel = !reviewCompleted ? "Start" : (dueCount > 0 ? "Again" : "Go on");
  const auto labels = mappedInput.mapLabels("Back", startLabel, "Swap", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void AnkiActivity::renderCardSide(const char* label) {
  const auto m = getCardMargins();

  // The label bar sits at the top of the content area, just inside the top margin
  const int topY = m.top - LABEL_HEIGHT;
  const int leftX = m.left;
  const int rightX = renderer.getScreenWidth() - m.right;

  // Top bar: label on left, status info on right
  renderer.drawText(SMALL_FONT_ID, leftX, topY, label);

  if (deck) {
    const char* sizeNames[] = {"S", "M", "L", "XL"};
    char statusStr[64];
    snprintf(statusStr, sizeof(statusStr), "%zu/%zu  %s  S%u",
             deck->getDuePosition() + 1, deck->getDueCount(),
             sizeNames[ankiFontSize], ANKI_SESSION.getSession());
    int statusW = renderer.getTextWidth(SMALL_FONT_ID, statusStr);
    renderer.drawText(SMALL_FONT_ID, rightX - statusW, topY, statusStr);
  }

  // Draw horizontal line under top bar
  const int lineY = topY + LABEL_HEIGHT - 5;
  renderer.drawLine(leftX, lineY, rightX, lineY);

  // Content area
  const int vpHeight = renderer.getScreenHeight() - m.top - m.bottom;

  // Render card content — vertically centered if single page and content fits
  if (!cardPages.empty() && currentCardPage < static_cast<int>(cardPages.size())) {
    int yOffset = m.top;
    if (cardPages.size() == 1 && cardContentHeight < vpHeight) {
      yOffset = m.top + (vpHeight - cardContentHeight) / 2;
    }
    cardPages[currentCardPage]->render(renderer, cachedFontId, leftX, yOffset);
  }

  // Button hints — grading buttons use raw front button order (not remapped),
  // because getPressedFrontButton() returns the raw physical index
  if (state == State::FRONT) {
    GUI.drawButtonHints(renderer, "", "", "", "");
  } else {
    GUI.drawButtonHints(renderer, "Again", "Hard", "Good", "Easy");
  }
}


