#pragma once

#include <cstdint>

class AnkiSessionManager {
  static AnkiSessionManager inst;

  uint32_t globalSession = 0;
  uint16_t cardsReviewedThisSession = 0;
  uint16_t totalDueThisSession = 0;

  AnkiSessionManager() = default;
  void ensureAnkixDir();

 public:
  AnkiSessionManager(const AnkiSessionManager&) = delete;
  AnkiSessionManager& operator=(const AnkiSessionManager&) = delete;

  static AnkiSessionManager& instance() { return inst; }

  void load();
  void save();

  uint32_t getSession() const { return globalSession; }
  uint16_t getCardsReviewed() const { return cardsReviewedThisSession; }
  uint16_t getTotalDue() const { return totalDueThisSession; }

  // Set total due cards (called by explorer after scanning)
  void setTotalDue(uint16_t n) { totalDueThisSession = n; }

  // Called after each card is graded. Returns true if session bumped.
  bool onCardReviewed();
};

#define ANKI_SESSION AnkiSessionManager::instance()
