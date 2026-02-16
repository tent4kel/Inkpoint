#include "AnkiSessionManager.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

namespace {
constexpr const char* SESSION_PATH = "/.ankix/global.session";
constexpr uint16_t BUMP_THRESHOLD = 10;
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
  serialization::readPod(file, cardsReviewedThisSession);
  file.close();
  LOG_DBG("ANK", "Loaded global session: %u, reviewed: %u", globalSession, cardsReviewedThisSession);
}

void AnkiSessionManager::save() {
  ensureAnkixDir();
  FsFile file;
  if (!Storage.openFileForWrite("ANK", SESSION_PATH, file)) {
    LOG_ERR("ANK", "Failed to save global session");
    return;
  }
  serialization::writePod(file, globalSession);
  serialization::writePod(file, cardsReviewedThisSession);
  file.close();
}

bool AnkiSessionManager::onCardReviewed() {
  cardsReviewedThisSession++;

  bool shouldBump = cardsReviewedThisSession >= BUMP_THRESHOLD;
  if (!shouldBump && totalDueThisSession > 0 && totalDueThisSession < BUMP_THRESHOLD) {
    shouldBump = cardsReviewedThisSession >= totalDueThisSession;
  }

  if (shouldBump) {
    globalSession++;
    cardsReviewedThisSession = 0;
    totalDueThisSession = 0;
    save();
    LOG_DBG("ANK", "Session bumped to %u", globalSession);
    return true;
  }

  save();
  return false;
}
