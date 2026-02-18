#include "AnkiDeck.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <esp_random.h>

#include <algorithm>
#include <functional>

#include "AnkiSessionManager.h"
#include "CsvParser.h"

constexpr const char* AnkiDeck::SM2_HEADERS[];

AnkiDeck::AnkiDeck(std::string csvPath) : csvPath(std::move(csvPath)) {}

bool AnkiDeck::load() {
  std::vector<CsvRow> rows;
  if (!CsvParser::parseFile(csvPath, rows)) {
    return false;
  }

  if (rows.size() < 2) {
    LOG_ERR("ANK", "CSV has no data rows");
    return false;
  }

  // Check if SM-2 columns already exist
  const auto& header = rows[0];
  bool hasSM2 = header.fields.size() >= TOTAL_COLS;

  cards.clear();
  cards.reserve(rows.size() - 1);

  for (size_t i = 1; i < rows.size(); i++) {
    auto& row = rows[i];
    if (row.fields.size() < 2) continue;

    FlashCard card;
    card.front = row.fields[COL_FRONT];
    card.back = row.fields[COL_BACK];

    if (hasSM2 && row.fields.size() >= TOTAL_COLS) {
      card.schedule.repetitions = static_cast<uint16_t>(atoi(row.fields[COL_REPS].c_str()));
      card.schedule.easinessFactor = static_cast<uint16_t>(atoi(row.fields[COL_EF].c_str()));
      card.schedule.interval = static_cast<uint32_t>(atol(row.fields[COL_INTERVAL].c_str()));
      card.schedule.nextReviewSession = static_cast<uint32_t>(atol(row.fields[COL_NEXT_SESSION].c_str()));
    }
    // Otherwise schedule stays at defaults (new card)

    cards.push_back(std::move(card));
  }

  // If no SM-2 columns, write them now
  if (!hasSM2) {
    LOG_DBG("ANK", "Adding SM-2 columns on first load");
    save();
  }

  LOG_DBG("ANK", "Loaded %zu cards from %s (global session %u)", cards.size(), csvPath.c_str(),
                ANKI_SESSION.getSession());
  return !cards.empty();
}

bool AnkiDeck::save() {
  std::vector<CsvRow> rows;
  rows.reserve(cards.size() + 1);

  // Header
  CsvRow header;
  header.fields = {"Front", "Back"};
  for (const auto& h : SM2_HEADERS) {
    header.fields.emplace_back(h);
  }
  rows.push_back(std::move(header));

  // Data rows
  for (const auto& card : cards) {
    CsvRow row;
    row.fields.resize(TOTAL_COLS);
    row.fields[COL_FRONT] = card.front;
    row.fields[COL_BACK] = card.back;
    row.fields[COL_REPS] = std::to_string(card.schedule.repetitions);
    row.fields[COL_EF] = std::to_string(card.schedule.easinessFactor);
    row.fields[COL_INTERVAL] = std::to_string(card.schedule.interval);
    row.fields[COL_NEXT_SESSION] = std::to_string(card.schedule.nextReviewSession);
    rows.push_back(std::move(row));
  }

  return CsvParser::writeFile(csvPath, rows);
}

void AnkiDeck::buildDueList() {
  const uint32_t session = ANKI_SESSION.getSession();
  dueIndices.clear();
  for (size_t i = 0; i < cards.size(); i++) {
    if (cards[i].schedule.nextReviewSession <= session) {
      dueIndices.push_back(i);
    }
  }

  // Fisher-Yates shuffle
  for (size_t i = dueIndices.size(); i > 1; i--) {
    size_t j = esp_random() % i;
    std::swap(dueIndices[i - 1], dueIndices[j]);
  }

  duePosition = 0;
  LOG_DBG("ANK", "Built due list: %zu cards due at session %u", dueIndices.size(), session);
}

void AnkiDeck::buildStudyAheadList() {
  const uint32_t session = ANKI_SESSION.getSession();
  dueIndices.clear();
  for (size_t i = 0; i < cards.size(); i++) {
    if (cards[i].schedule.nextReviewSession > session) {
      dueIndices.push_back(i);
    }
  }

  // Sort by nextReviewSession ascending (soonest due first)
  std::sort(dueIndices.begin(), dueIndices.end(), [this](size_t a, size_t b) {
    return cards[a].schedule.nextReviewSession < cards[b].schedule.nextReviewSession;
  });

  duePosition = 0;
  LOG_DBG("ANK", "Built study-ahead list: %zu future cards at session %u", dueIndices.size(), session);
}

FlashCard* AnkiDeck::currentCard() {
  if (duePosition >= dueIndices.size()) return nullptr;
  return &cards[dueIndices[duePosition]];
}

bool AnkiDeck::gradeCurrentCard(Grade grade) {
  if (duePosition >= dueIndices.size()) return false;

  const uint32_t session = ANKI_SESSION.getSession();
  auto& card = cards[dueIndices[duePosition]];
  card.schedule = SM2::review(card.schedule, grade, session);

  // If Again, re-queue this card at end of due list
  if (grade == Grade::Again) {
    dueIndices.push_back(dueIndices[duePosition]);
  }

  duePosition++;
  save();

  ANKI_SESSION.onCardReviewed();

  return duePosition < dueIndices.size();
}

uint32_t AnkiDeck::getCurrentSession() const {
  return ANKI_SESSION.getSession();
}

size_t AnkiDeck::countDueCards(const std::string& csvPath) {
  std::vector<CsvRow> rows;
  if (!CsvParser::parseFile(csvPath, rows)) {
    return 0;
  }

  if (rows.size() < 2) return 0;

  // Check if SM-2 columns exist
  const auto& header = rows[0];
  bool hasSM2 = header.fields.size() >= TOTAL_COLS;

  if (!hasSM2) {
    // All cards are new (due at session 0)
    return rows.size() - 1;
  }

  const uint32_t session = ANKI_SESSION.getSession();
  size_t count = 0;
  for (size_t i = 1; i < rows.size(); i++) {
    if (rows[i].fields.size() >= TOTAL_COLS) {
      uint32_t nextSession = static_cast<uint32_t>(atol(rows[i].fields[COL_NEXT_SESSION].c_str()));
      if (nextSession <= session) {
        count++;
      }
    }
  }
  return count;
}

std::string AnkiDeck::getTitle() const {
  size_t lastSlash = csvPath.find_last_of('/');
  std::string filename = (lastSlash != std::string::npos) ? csvPath.substr(lastSlash + 1) : csvPath;

  if (filename.length() >= 4 && filename.substr(filename.length() - 4) == ".csv") {
    filename = filename.substr(0, filename.length() - 4);
  }
  return filename;
}
