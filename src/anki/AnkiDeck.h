#pragma once

#include <string>
#include <vector>

#include "SM2.h"

struct FlashCard {
  std::string front;
  std::string back;
  CardSchedule schedule;
};

class AnkiDeck {
  std::string csvPath;
  std::vector<FlashCard> cards;
  std::vector<size_t> dueIndices;
  size_t duePosition = 0;

  // Column indices in CSV
  static constexpr int COL_FRONT = 0;
  static constexpr int COL_BACK = 1;
  static constexpr int COL_REPS = 2;
  static constexpr int COL_EF = 3;
  static constexpr int COL_INTERVAL = 4;
  static constexpr int COL_NEXT_SESSION = 5;
  static constexpr int TOTAL_COLS = 6;

  static constexpr const char* SM2_HEADERS[] = {"Repetitions", "EasinessFactor", "Interval", "NextReviewSession"};

 public:
  explicit AnkiDeck(std::string csvPath);

  bool load();
  bool save();

  // Build due list for current global session (shuffle included)
  void buildDueList();

  // Get current card for review. Returns nullptr if no more due.
  FlashCard* currentCard();

  // Grade current card and advance. Returns true if more cards remain.
  bool gradeCurrentCard(Grade grade);

  // Lightweight: parse CSV and count cards due at global session, without loading full deck
  static size_t countDueCards(const std::string& csvPath);

  uint32_t getCurrentSession() const;
  size_t getDueCount() const { return dueIndices.size(); }
  size_t getDuePosition() const { return duePosition; }
  size_t getTotalCards() const { return cards.size(); }
  size_t getRemainingCount() const { return duePosition < dueIndices.size() ? dueIndices.size() - duePosition : 0; }
  const std::string& getPath() const { return csvPath; }

  std::string getTitle() const;
};
