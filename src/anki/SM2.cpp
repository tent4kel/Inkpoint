#include "SM2.h"

#include <algorithm>

namespace SM2 {

CardSchedule review(const CardSchedule& card, Grade grade, uint32_t currentSession) {
  CardSchedule next = card;

  // Update easiness factor based on grade
  // Again: -200, Hard: -50, Good: +0, Easy: +100 (in thousandths)
  switch (grade) {
    case Grade::Again:
      next.easinessFactor = card.easinessFactor >= 1500 ? card.easinessFactor - 200 : 1300;
      break;
    case Grade::Hard:
      next.easinessFactor = card.easinessFactor >= 1350 ? card.easinessFactor - 50 : 1300;
      break;
    case Grade::Good:
      break;
    case Grade::Easy:
      next.easinessFactor = card.easinessFactor + 100;
      break;
  }
  next.easinessFactor = std::max(next.easinessFactor, static_cast<uint16_t>(1300));

  if (grade == Grade::Again) {
    // Failed: reset repetitions, show again this session
    next.repetitions = 0;
    next.interval = 0;
    next.nextReviewSession = currentSession;
  } else {
    // Passed: compute new interval
    if (card.repetitions == 0) {
      next.interval = 1;
    } else if (card.repetitions == 1) {
      next.interval = 6;
    } else {
      next.interval = static_cast<uint32_t>(card.interval * next.easinessFactor / 1000);
      if (next.interval < 1) next.interval = 1;
    }

    // Hard: reduce interval slightly (70% of computed)
    if (grade == Grade::Hard) {
      next.interval = std::max(static_cast<uint32_t>(1), next.interval * 7 / 10);
    }
    // Easy: boost interval (130% of computed)
    if (grade == Grade::Easy) {
      next.interval = next.interval * 13 / 10;
      if (next.interval < 2) next.interval = 2;
    }

    next.repetitions = card.repetitions + 1;
    next.nextReviewSession = currentSession + next.interval;
  }

  return next;
}

}  // namespace SM2
