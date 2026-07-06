#pragma once
#include <Preferences.h>

// ═══════════════════════════════════════════
//  NVS Credential Store
//  Persists WiFi + Spotify creds across reboots
// ═══════════════════════════════════════════

namespace NVStore {

static Preferences prefs;

struct Credentials {
  String wifiSSID;
  String wifiPass;
  String spClientId;
  String spClientSecret;
  String spRefreshToken;
  uint16_t screenTimeout;  // seconds, 0 = never
  uint8_t  brightness;      // 0-255, default 255
  uint8_t  wakePin;         // GPIO pin for wake button, default 0 (boot)
};

static Credentials creds;

inline void load() {
  prefs.begin("spotifytft", true);  // read-only
  creds.wifiSSID        = prefs.getString("wifi_ssid", "");
  creds.wifiPass        = prefs.getString("wifi_pass", "");
  creds.spClientId      = prefs.getString("sp_cid", "");
  creds.spClientSecret  = prefs.getString("sp_csec", "");
  creds.spRefreshToken  = prefs.getString("sp_rtok", "");
  creds.screenTimeout   = prefs.getUShort("scr_tout", 0);
  creds.brightness       = prefs.getUChar("bright", 255);
  creds.wakePin           = prefs.getUChar("wake_pin", 0);
  prefs.end();
}

inline bool hasWiFi() {
  return creds.wifiSSID.length() > 0;
}

inline bool hasSpotify() {
  return creds.spClientId.length() > 0 &&
         creds.spClientSecret.length() > 0 &&
         creds.spRefreshToken.length() > 0;
}

inline void saveWiFi(const String &ssid, const String &pass) {
  creds.wifiSSID = ssid;
  creds.wifiPass = pass;
  prefs.begin("spotifytft", false);
  prefs.putString("wifi_ssid", ssid);
  prefs.putString("wifi_pass", pass);
  prefs.end();
}

inline void saveSpotify(const String &cid, const String &csec, const String &rtok) {
  creds.spClientId = cid;
  creds.spClientSecret = csec;
  creds.spRefreshToken = rtok;
  prefs.begin("spotifytft", false);
  prefs.putString("sp_cid", cid);
  prefs.putString("sp_csec", csec);
  prefs.putString("sp_rtok", rtok);
  prefs.end();
}

inline void saveScreenTimeout(uint16_t seconds) {
  creds.screenTimeout = seconds;
  prefs.begin("spotifytft", false);
  prefs.putUShort("scr_tout", seconds);
  prefs.end();
}

inline void saveBrightness(uint8_t val) {
  creds.brightness = val;
  prefs.begin("spotifytft", false);
  prefs.putUChar("bright", val);
  prefs.end();
}

inline void saveWakePin(uint8_t pin) {
  creds.wakePin = pin;
  prefs.begin("spotifytft", false);
  prefs.putUChar("wake_pin", pin);
  prefs.end();
}

inline void clearAll() {
  prefs.begin("spotifytft", false);
  prefs.clear();
  prefs.end();
  creds = Credentials();
}

}  // namespace NVStore
