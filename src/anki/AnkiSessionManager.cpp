#include "AnkiSessionManager.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include "CrossPointSettings.h"

namespace {
constexpr const char* SESSION_PATH = "/.ankix/global.session";
}  // namespace

AnkiSessionManager AnkiSessionManager::inst;

void AnkiSessionManager::ensureAnkixDir() {
  if (!Storage.exists("/.ankix")) {
    Storage.mkdir("/.ankix");
  }
}

void AnkiSessionManager::load() {
  ensureAnkixDir();
  FsFile file;
  if (!Storage.openFileForRead("ANK", SESSION_PATH, file)) {
    globalSession = 0;
    cardsReviewedThisSession = 0;
    LOG_DBG("ANK", "No global session file, starting at 0");
    return;
  }
  serialization::readPod(file, globalSession);
  // cardsReviewedThisSession intentionally NOT restored — it resets to 0 on
  // every boot/power-cycle so the daily goal must be reached in one sitting.
  file.close();
  cardsReviewedThisSession = 0;
  LOG_DBG("ANK", "Loaded global session: %u (reviewed counter reset to 0)", globalSession);
}

void AnkiSessionManager::save() {
  ensureAnkixDir();
  FsFile file;
  if (!Storage.openFileForWrite("ANK", SESSION_PATH, file)) {
    LOG_ERR("ANK", "Failed to save global session");
    return;
  }
  serialization::writePod(file, globalSession);
  file.close();
}

void AnkiSessionManager::onCardReviewed() {
  cardsReviewedThisSession++;
  save();
}

void AnkiSessionManager::onCycleComplete() {
  // Bump session once per deck visit so that cards with interval=1
  // (Again, Hard, Good on first review) are due again next cycle.
  if (sessionBumpedThisRun) return;
  globalSession++;
  cardsReviewedThisSession = 0;
  totalDueThisSession = 0;
  sessionBumpedThisRun = true;
  save();
  LOG_DBG("ANK", "Session bumped to %u (cycle complete)", globalSession);
}
