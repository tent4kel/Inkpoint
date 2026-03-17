#include "SM2.h"

#include <algorithm>

namespace SM2 {

CardSchedule review(const CardSchedule& card, Grade grade, uint32_t currentSession,
                    uint16_t learningThreshold) {
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
    // Reset to start of learning phase; re-queued in the same session.
    next.repetitions = 0;
    next.interval = 0;
    next.nextReviewSession = currentSession;
  } else if (grade == Grade::Hard) {
    // Hard never advances repetitions in any phase — the card stays in its
    // current stage. Learning phase: always back next session (interval=1).
    // SM-2 phase: 30% interval reduction, same as classic SM-2 Hard.
    if (card.repetitions < learningThreshold) {
      next.interval = 1;
    } else {
      next.interval = std::max(static_cast<uint32_t>(1), card.interval * 7 / 10);
    }
    // next.repetitions unchanged (copied from card above)
    next.nextReviewSession = currentSession + next.interval;
  } else {
    // Good or Easy: advance repetitions.
    if (card.repetitions < learningThreshold && grade != Grade::Easy) {
      // Learning phase (Good only): fixed 1-session interval.
      // Easy bypasses this and uses SM-2 formula directly (see below).
      next.interval = 1;
    } else {
      // SM-2 phase, or Easy at any repetition count.
      // Easy uses EF-driven growth from the very first review; the first review
      // seeds at interval=0 which truncates to 2 via the max — giving one soft
      // check-in before the curve diverges from Good.
      next.interval = std::max(
          static_cast<uint32_t>(2),
          static_cast<uint32_t>(card.interval * next.easinessFactor / 1000));
      if (grade == Grade::Easy) {
        next.interval = std::max(static_cast<uint32_t>(2), next.interval * 13 / 10);
      }
    }
    next.repetitions = card.repetitions + 1;
    next.nextReviewSession = currentSession + next.interval;
  }

  return next;
}

}  // namespace SM2
