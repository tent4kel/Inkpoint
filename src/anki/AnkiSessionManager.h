#pragma once

#include <cstdint>

class AnkiSessionManager {
  static AnkiSessionManager inst;

  uint32_t globalSession = 0;
  uint16_t cardsReviewedThisSession = 0;
  uint16_t totalDueThisSession = 0;
  bool sessionBumpedThisRun = false;  // Runtime only — caps to one bump per explorer visit

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

  // Reset the per-visit bump cap (call when returning to explorer)
  void resetSessionBump() { sessionBumpedThisRun = false; }

  // Called after each card is graded. Returns true if session bumped.
  bool onCardReviewed();
};

#define ANKI_SESSION AnkiSessionManager::instance()
