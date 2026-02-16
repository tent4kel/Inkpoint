#pragma once

#include <cstdint>

struct CardSchedule {
  uint16_t repetitions = 0;
  uint16_t easinessFactor = 2500;    // EF * 1000 (2500 = 2.5)
  uint32_t interval = 0;            // Interval in sessions
  uint32_t nextReviewSession = 0;   // Session number when card is next due
};

enum class Grade : uint8_t {
  Again = 0,
  Hard = 1,
  Good = 2,
  Easy = 3,
};

namespace SM2 {

// Apply SM-2 algorithm. Returns updated schedule.
// All intervals are in sessions (not days) since the device has no reliable clock.
CardSchedule review(const CardSchedule& card, Grade grade, uint32_t currentSession);

}  // namespace SM2
