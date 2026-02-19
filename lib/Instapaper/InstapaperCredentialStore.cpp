#include "InstapaperCredentialStore.h"

#include <HalStorage.h>
#include <HardwareSerial.h>
#include <Serialization.h>

InstapaperCredentialStore InstapaperCredentialStore::instance;

namespace {
constexpr uint8_t FILE_VERSION = 2;
constexpr char CRED_FILE[] = "/.crosspoint/instapaper.bin";
constexpr uint8_t OBFUSCATION_KEY[] = {0x49, 0x6E, 0x73, 0x74, 0x61, 0x70, 0x61, 0x70};  // "Instapap"
constexpr size_t KEY_LENGTH = sizeof(OBFUSCATION_KEY);
}  // namespace

void InstapaperCredentialStore::obfuscate(std::string& data) const {
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= OBFUSCATION_KEY[i % KEY_LENGTH];
  }
}

bool InstapaperCredentialStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");

  FsFile file;
  if (!Storage.openFileForWrite("IPS", CRED_FILE, file)) {
    return false;
  }

  serialization::writePod(file, FILE_VERSION);

  std::string obfUsername = username;
  obfuscate(obfUsername);
  serialization::writeString(file, obfUsername);

  std::string obfPassword = password;
  obfuscate(obfPassword);
  serialization::writeString(file, obfPassword);

  std::string obfToken = token;
  obfuscate(obfToken);
  serialization::writeString(file, obfToken);

  std::string obfSecret = tokenSecret;
  obfuscate(obfSecret);
  serialization::writeString(file, obfSecret);

  serialization::writeString(file, downloadFolder);

  file.close();
  Serial.printf("[%lu] [IPS] Saved Instapaper credentials\n", millis());
  return true;
}

bool InstapaperCredentialStore::loadFromFile() {
  FsFile file;
  if (!Storage.openFileForRead("IPS", CRED_FILE, file)) {
    Serial.printf("[%lu] [IPS] No credentials file found\n", millis());
    return false;
  }

  uint8_t version;
  serialization::readPod(file, version);
  if (version != FILE_VERSION) {
    // Version 1 had no username/password, clear and start fresh
    Serial.printf("[%lu] [IPS] Old file version %u, resetting\n", millis(), version);
    file.close();
    return false;
  }

  if (file.available()) {
    serialization::readString(file, username);
    obfuscate(username);
  } else {
    username.clear();
  }

  if (file.available()) {
    serialization::readString(file, password);
    obfuscate(password);
  } else {
    password.clear();
  }

  if (file.available()) {
    serialization::readString(file, token);
    obfuscate(token);
  } else {
    token.clear();
  }

  if (file.available()) {
    serialization::readString(file, tokenSecret);
    obfuscate(tokenSecret);
  } else {
    tokenSecret.clear();
  }

  if (file.available()) {
    serialization::readString(file, downloadFolder);
  } else {
    downloadFolder = "/instapaper";
  }

  file.close();
  Serial.printf("[%lu] [IPS] Loaded Instapaper credentials (has tokens: %s)\n", millis(),
                hasCredentials() ? "yes" : "no");
  return true;
}

void InstapaperCredentialStore::setCredentials(const std::string& tok, const std::string& tokSecret) {
  token = tok;
  tokenSecret = tokSecret;
  Serial.printf("[%lu] [IPS] Set Instapaper credentials\n", millis());
}

bool InstapaperCredentialStore::hasCredentials() const { return !token.empty() && !tokenSecret.empty(); }

bool InstapaperCredentialStore::hasLoginCredentials() const { return !username.empty() && !password.empty(); }

void InstapaperCredentialStore::clearCredentials() {
  username.clear();
  password.clear();
  token.clear();
  tokenSecret.clear();
  saveToFile();
  Serial.printf("[%lu] [IPS] Cleared Instapaper credentials\n", millis());
}
