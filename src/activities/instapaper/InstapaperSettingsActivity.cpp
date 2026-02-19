#include "InstapaperSettingsActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <WiFi.h>
#include <ctime>

#include "network/InstapaperClient.h"
#include "InstapaperCredentialStore.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS_AUTH = 3;    // Username, Password, Authenticate
constexpr int MENU_ITEMS_AUTHED = 1;  // Clear credentials
const char* menuNamesAuth[] = {"Username", "Password", "Authenticate"};
const char* menuNamesAuthed[] = {"Clear Credentials"};
}  // namespace

void InstapaperSettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<InstapaperSettingsActivity*>(param);
  self->displayTaskLoop();
}

void InstapaperSettingsActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  selectedIndex = 0;
  statusMessage.clear();
  pendingUsername.clear();
  pendingPassword.clear();
  updateRequired = true;

  xTaskCreate(&InstapaperSettingsActivity::taskTrampoline, "InstaSettingsTask", 4096, this, 1, &displayTaskHandle);
}

void InstapaperSettingsActivity::onExit() {
  ActivityWithSubactivity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void InstapaperSettingsActivity::loop() {
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

  int itemCount = INSTAPAPER_STORE.hasCredentials() ? MENU_ITEMS_AUTHED : MENU_ITEMS_AUTH;

  buttonNavigator.onNext([this, itemCount] {
    selectedIndex = (selectedIndex + 1) % itemCount;
    updateRequired = true;
  });

  buttonNavigator.onPrevious([this, itemCount] {
    selectedIndex = (selectedIndex + itemCount - 1) % itemCount;
    updateRequired = true;
  });
}

void InstapaperSettingsActivity::handleSelection() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);

  if (INSTAPAPER_STORE.hasCredentials()) {
    if (selectedIndex == 0) {
      INSTAPAPER_STORE.clearCredentials();
      selectedIndex = 0;
      statusMessage = "Credentials cleared";
      updateRequired = true;
    }
    xSemaphoreGive(renderingMutex);
    return;
  }

  if (selectedIndex == 0) {
    // Username
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, "Instapaper Email", pendingUsername, 128, false,
        [this](const std::string& username) {
          pendingUsername = username;
          exitActivity();
          updateRequired = true;
        },
        [this]() {
          exitActivity();
          updateRequired = true;
        }));
  } else if (selectedIndex == 1) {
    // Password
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, "Instapaper Password", pendingPassword, 128, true,
        [this](const std::string& password) {
          pendingPassword = password;
          exitActivity();
          updateRequired = true;
        },
        [this]() {
          exitActivity();
          updateRequired = true;
        }));
  } else if (selectedIndex == 2) {
    // Authenticate
    if (pendingUsername.empty() || pendingPassword.empty()) {
      statusMessage = "Enter email and password first";
      updateRequired = true;
      xSemaphoreGive(renderingMutex);
      return;
    }

    statusMessage = "Connecting WiFi...";
    updateRequired = true;
    xSemaphoreGive(renderingMutex);

    // Connect WiFi if needed
    if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
      WiFi.mode(WIFI_STA);
      WiFi.begin();
      int wifiAttempts = 0;
      while (WiFi.status() != WL_CONNECTED && wifiAttempts < 100) {
        delay(100);
        wifiAttempts++;
      }
      if (WiFi.status() != WL_CONNECTED) {
        statusMessage = "WiFi not available";
        updateRequired = true;
        return;
      }
    }

    // Sync NTP time - OAuth signatures require accurate timestamps
    statusMessage = "Syncing clock...";
    updateRequired = true;
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    int retries = 0;
    while (time(nullptr) < 1000000000 && retries < 200) {
      delay(100);
      retries++;
    }
    LOG_DBG("IPS", "NTP time: %ld (retries: %d)", static_cast<long>(time(nullptr)), retries);

    if (time(nullptr) < 1000000000) {
      statusMessage = "Clock sync failed";
      updateRequired = true;
      return;
    }

    statusMessage = "Authenticating...";
    updateRequired = true;

    std::string token, tokenSecret;
    if (InstapaperClient::authenticate(pendingUsername, pendingPassword, token, tokenSecret)) {
      INSTAPAPER_STORE.setUsername(pendingUsername);
      INSTAPAPER_STORE.setPassword(pendingPassword);
      INSTAPAPER_STORE.setCredentials(token, tokenSecret);
      INSTAPAPER_STORE.saveToFile();
      statusMessage = "Authenticated!";
      selectedIndex = 0;
      pendingUsername.clear();
      pendingPassword.clear();
    } else {
      statusMessage = "Authentication failed";
    }
    updateRequired = true;
    return;
  }

  xSemaphoreGive(renderingMutex);
}

void InstapaperSettingsActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void InstapaperSettingsActivity::render() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Instapaper", true, EpdFontFamily::BOLD);

  bool hasAuth = INSTAPAPER_STORE.hasCredentials();
  int itemCount = hasAuth ? MENU_ITEMS_AUTHED : MENU_ITEMS_AUTH;
  const char** menuNames = hasAuth ? menuNamesAuthed : menuNamesAuth;

  renderer.fillRect(0, 60 + selectedIndex * 30 - 2, pageWidth - 1, 30);

  for (int i = 0; i < itemCount; i++) {
    const int settingY = 60 + i * 30;
    const bool isSelected = (i == selectedIndex);

    renderer.drawText(UI_10_FONT_ID, 20, settingY, menuNames[i], !isSelected);

    const char* status = "";
    if (!hasAuth) {
      if (i == 0) {
        status = pendingUsername.empty() ? "[Not Set]" : "[Set]";
      } else if (i == 1) {
        status = pendingPassword.empty() ? "[Not Set]" : "[Set]";
      }
    }

    if (strlen(status) > 0) {
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, status);
      renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, settingY, status, !isSelected);
    }
  }

  if (hasAuth) {
    renderer.drawCenteredText(UI_10_FONT_ID, 60 + itemCount * 30 + 20, "Authenticated");
  }

  if (!statusMessage.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 40, statusMessage.c_str());
  }

  const auto labels = mappedInput.mapLabels("« Back", "Select", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
