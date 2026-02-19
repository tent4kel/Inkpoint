#include "InstapaperOAuth.h"

#include <Arduino.h>
#include <base64.h>
#include <esp_random.h>
#include <mbedtls/md.h>

#include <ctime>

namespace {

std::string percentEncode(const std::string& s) {
  std::string result;
  result.reserve(s.size() * 2);
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      result += static_cast<char>(c);
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      result += buf;
    }
  }
  return result;
}

std::string generateNonce() {
  char buf[33];
  uint32_t r1 = esp_random();
  uint32_t r2 = esp_random();
  uint32_t r3 = esp_random();
  uint32_t r4 = esp_random();
  snprintf(buf, sizeof(buf), "%08x%08x%08x%08x", r1, r2, r3, r4);
  return buf;
}

std::string hmacSha1(const std::string& key, const std::string& data) {
  uint8_t result[20];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), reinterpret_cast<const uint8_t*>(key.data()), key.size(),
                  reinterpret_cast<const uint8_t*>(data.data()), data.size(), result);
  return std::string(reinterpret_cast<char*>(result), 20);
}

}  // namespace

std::string InstapaperOAuth::sign(const std::string& method, const std::string& url,
                                  const std::map<std::string, std::string>& params, const std::string& consumerKey,
                                  const std::string& consumerSecret, const std::string& token,
                                  const std::string& tokenSecret) {
  std::string timestamp = std::to_string(static_cast<long>(time(nullptr)));
  std::string nonce = generateNonce();

  // Build oauth params
  std::map<std::string, std::string> oauthParams;
  oauthParams["oauth_consumer_key"] = consumerKey;
  oauthParams["oauth_signature_method"] = "HMAC-SHA1";
  oauthParams["oauth_timestamp"] = timestamp;
  oauthParams["oauth_nonce"] = nonce;
  oauthParams["oauth_version"] = "1.0";
  if (!token.empty()) {
    oauthParams["oauth_token"] = token;
  }

  // Combine all params (oauth + request) - map is already sorted
  std::map<std::string, std::string> allParams = oauthParams;
  for (const auto& p : params) {
    allParams[p.first] = p.second;
  }

  // Build sorted parameter string
  std::string paramStr;
  for (const auto& p : allParams) {
    if (!paramStr.empty()) paramStr += "&";
    paramStr += percentEncode(p.first) + "=" + percentEncode(p.second);
  }

  // Build signature base string
  std::string baseString = method + "&" + percentEncode(url) + "&" + percentEncode(paramStr);

  // Build signing key
  std::string signingKey = percentEncode(consumerSecret) + "&" + percentEncode(tokenSecret);

  // Sign
  std::string signature = hmacSha1(signingKey, baseString);
  String sig64 = base64::encode(reinterpret_cast<const uint8_t*>(signature.data()), signature.size());

  oauthParams["oauth_signature"] = sig64.c_str();

  // Build Authorization header
  std::string header = "OAuth ";
  bool first = true;
  for (const auto& p : oauthParams) {
    if (!first) header += ", ";
    header += p.first + "=\"" + percentEncode(p.second) + "\"";
    first = false;
  }

  return header;
}
