#include "AnkiDeck.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <esp_random.h>

#include <algorithm>
#include <functional>

#include "AnkiSessionManager.h"
#include "CrossPointSettings.h"
#include "CsvParser.h"

constexpr const char* AnkiDeck::SM2_HEADERS[];

AnkiDeck::AnkiDeck(std::string csvPath) : csvPath(std::move(csvPath)) {}

bool AnkiDeck::load() {
  FsFile file;
  if (!Storage.openFileForRead("ANK", csvPath, file)) {
    return false;
  }

  const char delim = CsvParser::delimiterForPath(csvPath);

  // Stream line-by-line to avoid a large contiguous malloc for the whole file.
  // CsvParser::parseLine() is called per line so only one row is in memory at a time.
  constexpr size_t LINE_BUF_SIZE = 2048;
  auto* lineBuf = static_cast<char*>(malloc(LINE_BUF_SIZE));
  if (!lineBuf) {
    file.close();
    LOG_ERR("ANK", "OOM: line buffer");
    return false;
  }

  constexpr size_t CHUNK = 512;
  char chunk[CHUNK];
  size_t lineLen = 0;
  bool inQuotes  = false;
  bool isFirstRow = true;
  bool hasSM2    = false;

  cards.clear();

  auto processLine = [&](size_t len) {
    if (len == 0) return;
    if (lineBuf[len - 1] == '\r') len--;
    if (len == 0) return;

    CsvRow row = CsvParser::parseLine(lineBuf, len, delim);

    if (isFirstRow) {
      isFirstRow = false;
      hasSM2 = row.fields.size() >= TOTAL_COLS;
      return;
    }
    if (row.fields.size() < 2) return;

    FlashCard card;
    card.front = row.fields[COL_FRONT];
    card.back  = row.fields[COL_BACK];
    if (hasSM2 && row.fields.size() >= TOTAL_COLS) {
      card.schedule.repetitions       = static_cast<uint16_t>(atoi(row.fields[COL_REPS].c_str()));
      card.schedule.easinessFactor    = static_cast<uint16_t>(atoi(row.fields[COL_EF].c_str()));
      card.schedule.interval          = static_cast<uint32_t>(atol(row.fields[COL_INTERVAL].c_str()));
      card.schedule.nextReviewSession = static_cast<uint32_t>(atol(row.fields[COL_NEXT_SESSION].c_str()));
    }
    cards.push_back(std::move(card));
  };

  while (file.available()) {
    int n = file.read(chunk, CHUNK);
    if (n <= 0) break;
    for (int i = 0; i < n; i++) {
      const char c = chunk[i];
      if (c == '"') inQuotes = !inQuotes;
      if (!inQuotes && c == '\n') {
        processLine(lineLen);
        lineLen = 0;
      } else if (lineLen < LINE_BUF_SIZE - 1) {
        lineBuf[lineLen++] = c;
      }
    }
  }
  if (lineLen > 0) processLine(lineLen);  // last line without trailing newline

  free(lineBuf);
  file.close();

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

  return CsvParser::writeFile(csvPath, rows, CsvParser::delimiterForPath(csvPath));
}

void AnkiDeck::buildDueList() {
  const uint32_t session = ANKI_SESSION.getSession();
  // Scan cards in CSV order and stop at pool limit. New cards (nextReviewSession=0)
  // are always due; reviewed cards only when their interval has elapsed.
  // This creates a gradual introduction effect for large decks: cards near the
  // top of the CSV fill the pool first, and the frontier advances as those cards
  // get spaced out by SM-2. Unlimited (poolSize=0) preserves the original behaviour.
  const uint16_t poolSize = SETTINGS.getPoolSizeValue();
  dueIndices.clear();
  for (size_t i = 0; i < cards.size(); i++) {
    if (cards[i].schedule.nextReviewSession <= session) {
      dueIndices.push_back(i);
      if (poolSize > 0 && dueIndices.size() >= poolSize) break;
    }
  }

  // Fisher-Yates shuffle
  for (size_t i = dueIndices.size(); i > 1; i--) {
    size_t j = esp_random() % i;
    std::swap(dueIndices[i - 1], dueIndices[j]);
  }

  duePosition = 0;
  LOG_DBG("ANK", "Built due list: %zu cards (pool %u, session %u)", dueIndices.size(), poolSize, session);
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
  // Unlimited pool = no artificial learning phase; pure SM-2 from first review.
  const uint16_t threshold = SETTINGS.getPoolSizeValue() == 0 ? 0 : SM2::LEARNING_REPS;
  auto& card = cards[dueIndices[duePosition]];
  card.schedule = SM2::review(card.schedule, grade, session, threshold);

  duePosition++;
  save();

  ANKI_SESSION.onCardReviewed();

  return duePosition < dueIndices.size();
}

uint32_t AnkiDeck::getCurrentSession() const {
  return ANKI_SESSION.getSession();
}

size_t AnkiDeck::countDueCards(const std::string& csvPath) {
  // Streaming implementation: uses only stack buffers, no heap allocation.
  // This avoids std::bad_alloc when the heap is fragmented after studying.
  FsFile file;
  if (!Storage.openFileForRead("ANK", csvPath, file)) {
    return 0;
  }

  const char delim = CsvParser::delimiterForPath(csvPath);
  const uint32_t session = ANKI_SESSION.getSession();

  bool isFirstRow = true;
  bool hasSM2 = false;
  size_t count = 0;
  size_t totalDataRows = 0;

  // Per-character CSV state machine
  int  fieldIdx          = 0;
  bool inQuotes          = false;
  bool prevWasCloseQuote = false;
  char numBuf[16];
  int  numLen = 0;
  bool rowHasData = false;

  constexpr size_t CHUNK = 1024;
  char chunk[CHUNK];

  while (file.available()) {
    int n = file.read(chunk, CHUNK);
    if (n <= 0) break;
    for (int i = 0; i < n; i++) {
      const char c = chunk[i];

      // --- Quote handling (handles "" escape) ---
      if (prevWasCloseQuote) {
        prevWasCloseQuote = false;
        if (c == '"') {
          // Escaped quote inside quoted field — re-enter quote mode, skip char
          inQuotes = true;
          continue;
        }
        // Fall through: c is first char after closing quote, process normally
      }

      if (inQuotes) {
        if (c == '"') { inQuotes = false; prevWasCloseQuote = true; }
        // All other chars inside quotes are skipped (we don't need their value)
        continue;
      }

      // --- Normal (non-quoted) mode ---
      if (c == '"' && numLen == 0) {
        inQuotes = true;
        continue;
      }

      if (c == '\r') continue;

      if (c == delim) {
        fieldIdx++;
        numLen = 0;
        continue;
      }

      if (c == '\n') {
        if (rowHasData) {
          if (isFirstRow) {
            hasSM2 = (fieldIdx + 1) >= TOTAL_COLS;
            isFirstRow = false;
          } else {
            totalDataRows++;
            if (hasSM2 && fieldIdx == TOTAL_COLS - 1 && numLen > 0) {
              numBuf[numLen] = '\0';
              uint32_t nextSession = static_cast<uint32_t>(atol(numBuf));
              if (nextSession <= session) count++;
            }
          }
        }
        // Reset for next row
        fieldIdx  = 0;
        numLen    = 0;
        rowHasData = false;
        continue;
      }

      rowHasData = true;
      // Accumulate only the NextReviewSession field (index TOTAL_COLS-1)
      if (fieldIdx == TOTAL_COLS - 1 && numLen < static_cast<int>(sizeof(numBuf)) - 1) {
        numBuf[numLen++] = c;
      }
    }
  }

  // Handle last row if file doesn't end with newline
  if (rowHasData && !isFirstRow) {
    totalDataRows++;
    if (hasSM2 && fieldIdx == TOTAL_COLS - 1 && numLen > 0) {
      numBuf[numLen] = '\0';
      uint32_t nextSession = static_cast<uint32_t>(atol(numBuf));
      if (nextSession <= session) count++;
    }
  }

  file.close();
  LOG_DBG("ANK", "countDueCards(%s): %zu due / %zu total", csvPath.c_str(), count, totalDataRows);
  return hasSM2 ? count : totalDataRows;
}

size_t AnkiDeck::countAllDue() const {
  const uint32_t session = ANKI_SESSION.getSession();
  size_t count = 0;
  for (const auto& card : cards) {
    if (card.schedule.nextReviewSession <= session) count++;
  }
  return count;
}

std::string AnkiDeck::getTitle() const {
  size_t lastSlash = csvPath.find_last_of('/');
  std::string filename = (lastSlash != std::string::npos) ? csvPath.substr(lastSlash + 1) : csvPath;

  if (filename.size() >= 4 && (filename.substr(filename.size() - 4) == ".csv" ||
                                filename.substr(filename.size() - 4) == ".tsv")) {
    filename = filename.substr(0, filename.size() - 4);
  }
  return filename;
}
