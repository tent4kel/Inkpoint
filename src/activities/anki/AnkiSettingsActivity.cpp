#include "AnkiSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 2;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_LEARNING_STACK_LIMIT, StrId::STR_ANKI_DAILY_GOAL};

const StrId kStackLabels[] = {StrId::STR_POOL_10, StrId::STR_POOL_20, StrId::STR_POOL_30, StrId::STR_POOL_50,
                              StrId::STR_POOL_UNLIMITED};
const StrId kGoalLabels[] = {StrId::STR_GOAL_5, StrId::STR_GOAL_10, StrId::STR_GOAL_15,
                             StrId::STR_GOAL_20, StrId::STR_GOAL_30, StrId::STR_GOAL_50};
}  // namespace

void AnkiSettingsActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void AnkiSettingsActivity::onExit() { ActivityWithSubactivity::onExit(); }

void AnkiSettingsActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    requestUpdate();
  });
}

void AnkiSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    SETTINGS.ankiPoolSize = (SETTINGS.ankiPoolSize + 1) % CrossPointSettings::ANKI_POOL_SIZE_COUNT;
  } else {
    SETTINGS.ankiDailyGoal = (SETTINGS.ankiDailyGoal + 1) % CrossPointSettings::ANKI_DAILY_GOAL_COUNT;
  }
  SETTINGS.saveToFile();
  requestUpdate();
}

void AnkiSettingsActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_ANKI));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, MENU_ITEMS, static_cast<int>(selectedIndex),
      [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr, nullptr,
      [](int index) {
        if (index == 0) {
          const auto i = SETTINGS.ankiPoolSize < CrossPointSettings::ANKI_POOL_SIZE_COUNT
                             ? SETTINGS.ankiPoolSize
                             : CrossPointSettings::POOL_20;
          return std::string(I18N.get(kStackLabels[i]));
        } else {
          const auto i = SETTINGS.ankiDailyGoal < CrossPointSettings::ANKI_DAILY_GOAL_COUNT
                             ? SETTINGS.ankiDailyGoal
                             : CrossPointSettings::GOAL_10;
          return std::string(I18N.get(kGoalLabels[i]));
        }
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
