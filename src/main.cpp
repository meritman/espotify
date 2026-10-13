#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TJpg_Decoder.h>

#include "config.h"
#include "data_structs.h"
#include "nvs_store.h"

SemaphoreHandle_t uiMutex;

// Spotify credentials loaded from NVS at boot
String spotifyAccessToken = "";
uint32_t tokenExpirationTime = 0;

// Globals
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite sprite = TFT_eSprite(&tft);

#include "ui_volos.h"

SpotifyData spotify   = {};

// Smooth progress & lyrics interpolation
unsigned long lastProgressUpdateMillis = 0;
int lastKnownProgressMs = 0;
int lastKnownDurationMs = 0;
// 

uint32_t lastUpdate    = 0;
bool     forceRedraw   = true;

uint16_t albumArt[15616] = {0}; // 128x122 buffer for background cover art

// Screen Timeout
uint32_t lastMusicActivityMs = 0;  // tracks when music was last playing
bool     screenOff = false;

// Factory Reset Button
#define RESET_BTN_PIN  12
#define RESET_HOLD_MS  3000
uint32_t btnPressStart = 0;
bool     btnWasPressed = false;

bool tjpg_callback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= 64 || x >= 64) return 1;
  for (int16_t j = 0; j < h; j++) {
    for (int16_t i = 0; i < w; i++) {
      // Get the raw color from TJpgDec (outputting BGR565)
      uint16_t raw = bitmap[j * w + i];
      
      // Swap the Red and Blue channels to convert BGR565 into true RGB565
      uint16_t color = ((raw & 0x001F) << 11) | (raw & 0x07E0) | ((raw & 0xF800) >> 11);
      
      int px = x + i;
      int py = y + j;
      
      // Calculate a smooth vertical vignette (darker at top and bottom, brighter in the middle)
      int distY = abs(py - 32); // 0 to 32
      // Quadratic falloff: center = 110 alpha, edges = 20 alpha
      uint8_t alpha = 20 + (90 * (1024 - distY * distY)) / 1024;
      
      // High-precision alpha blend with the vignette alpha
      uint16_t dimColor = tft.alphaBlend(alpha, color, 0x0000); 
      uint16_t swappedColor = (dimColor >> 8) | (dimColor << 8);
      
      // Scale 64x64 to 128x120 (fits exactly between y=24 and y=144)
      int startX = px * 2;
      int endX = startX + 2;
      
      int startY = (py * 120) / 64;
      int endY = ((py + 1) * 120) / 64;
      
      for (int sy = startY; sy < endY; sy++) {
        for (int sx = startX; sx < endX; sx++) {
           if (sx < 128 && sy < 120) {
             albumArt[sy * 128 + sx] = swappedColor; 
           }
        }
      }
    }
  }
  return 1;
}

// LRC Sync Globals
#define MAX_LYRICS 180
struct LyricLine {
  uint32_t timeMs;
  String text;
};
LyricLine lyrics[MAX_LYRICS];
int numLyrics = 0;
String currentTrackForLyrics = "";

bool isFadingLyric = false;
bool fadeDirectionIn = false;
String targetLyric1 = "";
String targetLyric2 = "";

//  Prototypes
void refreshSpotifyToken();
void pollSpotifyAPI();
void fetchLyrics(String track, String artist, String album = "", uint32_t durationMs = 0);
void updateSyncedLyric();
void spotifyTask(void *pvParameters);
void applyScreenTimeout(uint16_t seconds);

inline void blWrite(uint8_t val) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_BL, val);
#else
  ledcWrite(BL_CHANNEL, val);
#endif
}

// External Assets
const char b64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
String base64Encode(String text) {
  String encoded = "";
  encoded.reserve((text.length() + 2) / 3 * 4 + 1);
  int i = 0, j = 0;
  uint8_t char_array_3[3], char_array_4[4];
  for (int idx = 0; idx < text.length(); idx++) {
    char_array_3[i++] = text[idx];
    if (i == 3) {
      char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
      char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
      char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
      char_array_4[3] = char_array_3[2] & 0x3f;
      for (i = 0; (i < 4); i++) encoded += b64_alphabet[char_array_4[i]];
      i = 0;
    }
  }
  if (i > 0) {
    for (j = i; j < 3; j++) char_array_3[j] = '\0';
    char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
    char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
    char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
    char_array_4[3] = char_array_3[2] & 0x3f;
    for (j = 0; (j < i + 1); j++) encoded += b64_alphabet[char_array_4[j]];
    while ((i++ < 3)) encoded += '=';
  }
  return encoded;
}

// Include web portal after base64Encode and spotifyAccessToken are defined
#include "web_portal.h"

// Screen Timeout
void applyScreenTimeout(uint16_t seconds) {
  // Just save the value - actual timeout logic runs in loop()
  lastMusicActivityMs = millis();
  if (screenOff) {
    screenOff = false;
    blWrite(NVStore::creds.brightness);
  }
}

void applyBrightness(uint8_t val) {
  if (!screenOff) {
    blWrite(val);
  }
}

void wakeScreen() {
  if (screenOff) {
    screenOff = false;
    blWrite(NVStore::creds.brightness);
    forceRedraw = true;
  }
  lastMusicActivityMs = millis();
}

// Setup - NVS-Driven Boot Flow
static uint32_t apShutdownTime = 0;
static bool spotifyTaskStarted = false;

void setup() {
  Serial.begin(115200);
  
  uiMutex = xSemaphoreCreateMutex();

  pinMode(RESET_BTN_PIN, INPUT_PULLUP);

  // Wake button setup (configurable GPIO, loaded after NVS)

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_BL, BL_FREQ, BL_RES);
#else
  ledcSetup(BL_CHANNEL, BL_FREQ, BL_RES);
  ledcAttachPin(PIN_BL, BL_CHANNEL);
#endif
  blWrite(BL_FULL);

  tft.begin();
  tft.setRotation(SCREEN_ROT);
  tft.fillScreen(TFT_BLACK);
  
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(tjpg_callback);

  sprite.createSprite(128, 160);

// Load NVS Credentials
  NVStore::load();

  if (!NVStore::hasWiFi()) {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextFont(2);
    tft.drawString("ESPotify", 10, 10);
    tft.setTextFont(1);
    tft.setTextColor(0x07E0, TFT_BLACK);
    tft.drawString("Setup Mode", 10, 32);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Connect WiFi to:", 10, 52);
    tft.setTextColor(0x07FF, TFT_BLACK);
    tft.drawString("ESPotify-Setup", 10, 66);
    tft.setTextColor(0x7BEF, TFT_BLACK);
    tft.drawString("Then open browser", 10, 86);

    Portal::startAP();
    Serial.println("[Boot] No WiFi creds → AP mode");
    return;  // Don't start Spotify task yet
  }

// WiFi Credentials Exist → Connect
  tft.setTextColor(TFT_WHITE);
  tft.drawString("WiFi...", 10, 10);
  
  WiFi.begin(NVStore::creds.wifiSSID.c_str(), NVStore::creds.wifiPass.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    tft.print(".");
    attempts++;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    // WiFi failedfall back to AP mode
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(0xF800, TFT_BLACK);
    tft.drawString("WiFi Failed!", 10, 10);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextFont(1);
    tft.drawString("Starting setup...", 10, 30);
    
    NVStore::clearAll();  // Clear bad creds
    delay(1500);
    Portal::startAP();
    Serial.println("[Boot] WiFi failed → AP mode");
    return;
  }

  tft.fillScreen(TFT_BLACK);
  tft.setCursor(10, 10);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.println("WiFi Connected!");
  tft.println(WiFi.localIP());

  if (!NVStore::hasSpotify()) {
// PHASE 2: WiFi OK but no Spotify → Setup Page
    tft.setTextColor(0x07E0, TFT_BLACK);
    tft.println("");
    tft.println("Visit to setup:");
    tft.setTextColor(0x07FF, TFT_BLACK);
    tft.println(WiFi.localIP());
    
    Portal::startSTA();
    Serial.println("[Boot] No Spotify creds → serving setup page");
    return;  // Don't start Spotify task yet
  }
  delay(1000);

  // Start web server for controls
  Portal::startSTA();

  // Apply saved brightness
  blWrite(NVStore::creds.brightness);

  // Setup wake button pin
  if (NVStore::creds.wakePin != RESET_BTN_PIN) {
    pinMode(NVStore::creds.wakePin, INPUT_PULLUP);
  }

  // Initialize activity timer
  lastMusicActivityMs = millis();
  
  // Launch Spotify Polling Task on Core 1
  xTaskCreatePinnedToCore(
    spotifyTask,        // Task function
    "SpotifyTask",      // Task name
    12288,              // Stack size (bytes)
    NULL,               // Parameters
    1,                  // Priority
    NULL,               // Task handle
    1                   // Core 1 (UI Core)
  );
  spotifyTaskStarted = true;
}

// Loop
void loop() {
  uint32_t now = millis();

// Web Server (always runs)
  Portal::loop();

// AP Auto-Shutdown after WiFi connect
  if (Portal::setupComplete && Portal::apActive) {
    if (apShutdownTime == 0) {
      apShutdownTime = now + 5000;  // 5 seconds
    } else if (now >= apShutdownTime) {
      Portal::shutdownAP();
      apShutdownTime = 0;
      
      // If Spotify creds now exist, start Spotify task
      NVStore::load();
      if (NVStore::hasSpotify() && !spotifyTaskStarted) {
        xTaskCreatePinnedToCore(spotifyTask, "SpotifyTask", 12288, NULL, 1, NULL, 1);
        spotifyTaskStarted = true;
      }
    }
  }

// Start Spotify task when creds become available
  if (!spotifyTaskStarted && NVStore::hasSpotify() && WiFi.status() == WL_CONNECTED) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(0x07E0, TFT_BLACK);
    tft.drawString("Setup Complete!", 10, 10);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextFont(1);
    tft.drawString("Starting Spotify...", 10, 30);
    delay(1500);
    lastMusicActivityMs = millis();
    xTaskCreatePinnedToCore(spotifyTask, "SpotifyTask", 12288, NULL, 1, NULL, 1);
    spotifyTaskStarted = true;
  }

// If Spotify not configured, skip rendering
  if (!spotifyTaskStarted) {
    return;
  }

// Factory Reset: Long-Press Button
  if (digitalRead(RESET_BTN_PIN) == LOW) {
    if (!btnWasPressed) {
      btnWasPressed = true;
      btnPressStart = now;
    } else if (now - btnPressStart >= RESET_HOLD_MS) {
      // Show reset message on TFT
      sprite.fillSprite(TFT_BLACK);
      sprite.setTextDatum(TC_DATUM);
      sprite.setTextFont(2);
      sprite.setTextColor(0xF800);
      sprite.drawString("FACTORY RESET", 64, 60);
      sprite.setTextFont(1);
      sprite.setTextColor(TFT_WHITE);
      sprite.drawString("Rebooting...", 64, 85);
      sprite.pushSprite(0, 0);
      delay(1500);
      NVStore::clearAll();
      ESP.restart();
    }
  } else {
    btnWasPressed = false;
  }

// Screen Timeout (based on music activity)
  if (NVStore::creds.screenTimeout > 0 && !screenOff) {
    // Only sleep if music is NOT playing AND timeout elapsed
    if (!spotify.isPlaying) {
      uint32_t currentMs = millis();
      // Ensure we don't underflow if currentMs < lastMusicActivityMs due to race conditions
      if (currentMs >= lastMusicActivityMs && (currentMs - lastMusicActivityMs >= (uint32_t)NVStore::creds.screenTimeout * 1000)) {
        screenOff = true;
        blWrite(BL_OFF);
      }
    } else {
      // Music is playing - keep resetting the timer
      lastMusicActivityMs = millis();
    }
  }

// Wake button check
  if (screenOff) {
    uint8_t wPin = NVStore::creds.wakePin;
    if (digitalRead(wPin) == LOW) {
      wakeScreen();
    }
  }

// Spotify UI Rendering
  if (screenOff) return;  // Skip rendering when screen is off
  
// Real-time Spotify Progress Interpolation & Lyric Checking
  if (lastKnownDurationMs > 0) {
    int interpolated = lastKnownProgressMs + (now - lastProgressUpdateMillis);
    if (interpolated > lastKnownDurationMs) {
      interpolated = lastKnownDurationMs;
    }
    spotify.progressMs = interpolated;
    
    // Check and trigger line transitions immediately on the interpolated timer
    updateSyncedLyric(); 
  }
// 

  xSemaphoreTake(uiMutex, portMAX_DELAY);
  
  sprite.setTextFont(2);
  bool isTitleScrolling = (sprite.textWidth(spotify.title) > 124);
  
  // Trigger fluid UI frames if we are actively viewing the playing Spotify layout
  bool isSpotifyAnimating = lastKnownDurationMs > 0;
  bool needsAnimation = isFadingLyric || isTitleScrolling || isSpotifyAnimating;

  // Draw screen every 1s OR if an animation is playing
  if (now - lastUpdate >= UPDATE_INTERVAL || forceRedraw || needsAnimation) {
    if (needsAnimation) {
       // Cap animation to ~30 FPS (33ms per frame)
       if (now - lastUpdate < 33) {
         xSemaphoreGive(uiMutex);
         return;
       }
    }
    
    if (isFadingLyric) {
       if (!fadeDirectionIn) {
         if (spotify.lyricAlpha >= 25) spotify.lyricAlpha -= 25;
         else {
           spotify.lyricAlpha = 0;
           fadeDirectionIn = true; // Hit bottom, swap text and fade back up
           strncpy(spotify.currentLyric1, targetLyric1.c_str(), sizeof(spotify.currentLyric1)-1);
           strncpy(spotify.currentLyric2, targetLyric2.c_str(), sizeof(spotify.currentLyric2)-1);
         }
       } else {
         if (spotify.lyricAlpha <= 230) spotify.lyricAlpha += 25;
         else {
           spotify.lyricAlpha = 255;
           isFadingLyric = false; // Animation finished
         }
       }
    }
    
    lastUpdate = now;
    forceRedraw = false;
    
    drawSpotify(spotify);

    sprite.pushSprite(0, 0);
  }
  
  xSemaphoreGive(uiMutex);
}

// LRC Parsing & Sanitization Logic
String urlEncode(const String& str) {
  String encodedString = "";
  encodedString.reserve(str.length() * 3);
  const char hexChars[] = "0123456789ABCDEF";
  for (size_t i = 0; i < str.length(); i++) {
    uint8_t c = (uint8_t)str.charAt(i);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      encodedString += (char)c;
    } else {
      encodedString += '%';
      encodedString += hexChars[(c >> 4) & 0x0F];
      encodedString += hexChars[c & 0x0F];
    }
  }
  return encodedString;
}

String cleanTrackTitle(String title) {
  // Normalize quotes and dashes
  title.replace("’", "'");
  title.replace("`", "'");
  title.replace("“", "\"");
  title.replace("”", "\"");
  title.replace("–", "-");
  title.replace("—", "-");

  // Cut common suffixes like " - Remastered", " - Live", " - Radio Edit", etc.
  int dashIdx = title.indexOf(" - ");
  if (dashIdx != -1) {
    String suffix = title.substring(dashIdx + 3);
    suffix.toLowerCase();
    if (suffix.indexOf("remaster") != -1 ||
        suffix.indexOf("live") != -1 ||
        suffix.indexOf("radio edit") != -1 ||
        suffix.indexOf("single version") != -1 ||
        suffix.indexOf("bonus") != -1 ||
        suffix.indexOf("deluxe") != -1 ||
        suffix.indexOf("mix") != -1 ||
        suffix.indexOf("version") != -1 ||
        suffix.indexOf("feat") != -1 ||
        suffix.indexOf("anniversary") != -1) {
      title = title.substring(0, dashIdx);
    }
  }

  // Remove feat/with/version inside brackets e.g. "(feat. ...)" or "[feat. ...]"
  auto stripPattern = [](String& str, const char* openB, const char* closeB) {
    int openIdx = str.indexOf(openB);
    while (openIdx != -1) {
      int closeIdx = str.indexOf(closeB, openIdx);
      if (closeIdx != -1) {
        String inside = str.substring(openIdx + 1, closeIdx);
        inside.toLowerCase();
        if (inside.indexOf("feat") != -1 ||
            inside.indexOf("with ") != -1 ||
            inside.indexOf("remaster") != -1 ||
            inside.indexOf("live") != -1 ||
            inside.indexOf("bonus") != -1 ||
            inside.indexOf("version") != -1) {
          str = str.substring(0, openIdx) + str.substring(closeIdx + 1);
          str.trim();
          openIdx = str.indexOf(openB);
          continue;
        }
      }
      openIdx = str.indexOf(openB, openIdx + 1);
    }
  };

  stripPattern(title, "(", ")");
  stripPattern(title, "[", "]");

  title.trim();
  return title;
}

String cleanArtistName(String artist) {
  int featIdx = artist.indexOf(" feat.");
  if (featIdx == -1) featIdx = artist.indexOf(" ft.");
  if (featIdx != -1) {
    artist = artist.substring(0, featIdx);
  }
  artist.trim();
  return artist;
}

void parseLrc(const String& lrc) {
  numLyrics = 0;
  int pos = 0;
  int len = lrc.length();

  while (pos < len && numLyrics < MAX_LYRICS) {
    int nextLine = lrc.indexOf('\n', pos);
    if (nextLine == -1) nextLine = len;

    String line = lrc.substring(pos, nextLine);
    pos = nextLine + 1;
    line.trim();
    if (line.length() == 0) continue;

    // Line might have multiple timestamp tags: [00:10.00][00:20.00]Chorus
    int textStart = 0;
    uint32_t timestamps[8];
    int tsCount = 0;

    int scanIdx = 0;
    while (scanIdx < line.length() && line[scanIdx] == '[') {
      int closeBracket = line.indexOf(']', scanIdx);
      if (closeBracket == -1) break;

      String tag = line.substring(scanIdx + 1, closeBracket);
      tag.trim();

      // Check if tag is timestamp: e.g. "01:23.45" or "01:23" or "1:23.456"
      int colonIdx = tag.indexOf(':');
      if (colonIdx > 0 && tsCount < 8) {
        String minStr = tag.substring(0, colonIdx);
        String secRest = tag.substring(colonIdx + 1);
        int dotIdx = secRest.indexOf('.');
        if (dotIdx == -1) dotIdx = secRest.indexOf(':');

        String secStr = (dotIdx != -1) ? secRest.substring(0, dotIdx) : secRest;
        String fracStr = (dotIdx != -1) ? secRest.substring(dotIdx + 1) : "";

        bool validTs = true;
        for (size_t c = 0; c < minStr.length(); c++) {
          if (!isDigit(minStr[c])) validTs = false;
        }
        for (size_t c = 0; c < secStr.length(); c++) {
          if (!isDigit(secStr[c])) validTs = false;
        }

        if (validTs && minStr.length() > 0 && secStr.length() > 0) {
          uint32_t m = minStr.toInt();
          uint32_t s = secStr.toInt();
          uint32_t ms = 0;
          if (fracStr.length() == 1) {
            ms = fracStr.toInt() * 100;
          } else if (fracStr.length() == 2) {
            ms = fracStr.toInt() * 10;
          } else if (fracStr.length() >= 3) {
            ms = fracStr.substring(0, 3).toInt();
          }

          timestamps[tsCount++] = (m * 60000) + (s * 1000) + ms;
          textStart = closeBracket + 1;
        }
      }
      scanIdx = closeBracket + 1;
      while (scanIdx < line.length() && line[scanIdx] == ' ') scanIdx++;
    }

    if (tsCount > 0) {
      String lyricText = "";
      if (textStart < line.length()) {
        lyricText = line.substring(textStart);
        lyricText.trim();
      }
      for (int t = 0; t < tsCount && numLyrics < MAX_LYRICS; t++) {
        lyrics[numLyrics].timeMs = timestamps[t];
        lyrics[numLyrics].text = lyricText;
        numLyrics++;
      }
    }
  }

  // Sort timestamps in ascending order if multi-timestamp tags were present
  for (int i = 0; i < numLyrics - 1; i++) {
    for (int j = i + 1; j < numLyrics; j++) {
      if (lyrics[j].timeMs < lyrics[i].timeMs) {
        LyricLine tmp = lyrics[i];
        lyrics[i] = lyrics[j];
        lyrics[j] = tmp;
      }
    }
  }
}

int executeLrclibGet(const String& url, String& responseBody) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(12000);

  HTTPClient http;
  http.begin(client, url);
  http.setUserAgent("ESPotify/1.0.0 (https://github.com/meritman/espotify)");
  http.addHeader("X-User-Agent", "ESPotify/1.0.0 (https://github.com/meritman/espotify)");
  http.addHeader("Lrclib-Client", "ESPotify/1.0.0 (https://github.com/meritman/espotify)");
  http.addHeader("Accept", "application/json");
  http.addHeader("Connection", "close");
  http.setTimeout(12000);

  const char* headerKeys[] = {"Retry-After"};
  http.collectHeaders(headerKeys, 1);

  int code = http.GET();

  // Handle 503 ServerOverloaded (retry once per LRCLIB documentation)
  if (code == 503) {
    String retryAfter = http.header("Retry-After");
    int waitMs = retryAfter.toInt() * 1000;
    if (waitMs <= 0 || waitMs > 3000) waitMs = 1000;
    http.end();
    delay(waitMs);

    http.begin(client, url);
    http.setUserAgent("ESPotify/1.0.0 (https://github.com/meritman/espotify)");
    http.addHeader("X-User-Agent", "ESPotify/1.0.0 (https://github.com/meritman/espotify)");
    http.addHeader("Lrclib-Client", "ESPotify/1.0.0 (https://github.com/meritman/espotify)");
    http.addHeader("Accept", "application/json");
    http.addHeader("Connection", "close");
    http.setTimeout(12000);
    code = http.GET();
  }

  if (code == 200) {
    responseBody = http.getString();
  }
  http.end();
  return code;
}

bool parseGetResult(const String& body, bool& isInstrumental) {
  isInstrumental = false;
  StaticJsonDocument<384> filter;
  filter["instrumental"] = true;
  filter["syncedLyrics"] = true;

  DynamicJsonDocument doc(6144);
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) return false;

  if (doc["instrumental"] == true) {
    isInstrumental = true;
    return true;
  }

  const char* synced = doc["syncedLyrics"];
  if (synced != nullptr && strlen(synced) >= 10) {
    parseLrc(String(synced));
    return (numLyrics > 0);
  }
  return false;
}

bool parseSearchResult(const String& body, uint32_t targetDurationSec, bool& isInstrumental) {
  isInstrumental = false;
  StaticJsonDocument<384> filter;
  filter[0]["instrumental"] = true;
  filter[0]["syncedLyrics"] = true;
  filter[0]["duration"] = true;

  DynamicJsonDocument doc(12288);
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err || !doc.is<JsonArray>()) return false;

  JsonArray arr = doc.as<JsonArray>();
  if (arr.size() == 0) return false;

  int bestIdx = -1;
  float minDurationDiff = 99999.0f;
  bool foundInst = false;

  for (size_t i = 0; i < arr.size(); i++) {
    JsonObject item = arr[i];
    if (item["instrumental"] == true) foundInst = true;

    const char* sLrc = item["syncedLyrics"];
    if (sLrc != nullptr && strlen(sLrc) >= 10) {
      if (targetDurationSec > 0) {
        float itemDur = item["duration"] | 0.0f;
        float diff = fabs(itemDur - (float)targetDurationSec);
        if (diff < minDurationDiff) {
          minDurationDiff = diff;
          bestIdx = (int)i;
        }
      } else if (bestIdx == -1) {
        bestIdx = (int)i;
      }
    }
  }

  if (bestIdx != -1) {
    const char* sLrc = arr[bestIdx]["syncedLyrics"];
    parseLrc(String(sLrc));
    return (numLyrics > 0);
  }

  if (foundInst) {
    isInstrumental = true;
    return true;
  }

  return false;
}

void splitLyric(String text, String &l1, String &l2) {
  l1 = ""; l2 = "";
  text.trim();
  if (text.length() <= 18) {
    l1 = text;
    return;
  }
  int mid = text.length() / 2;
  int splitIndex = -1;
  int minDiff = 999;
  for (int i = 0; i < text.length(); i++) {
    if (text[i] == ' ') {
      int diff = abs(i - mid);
      if (diff < minDiff) {
        minDiff = diff;
        splitIndex = i;
      }
    }
  }
  if (splitIndex == -1 || minDiff > text.length()/3) {
    if (text.length() > 18) {
       l1 = text.substring(0, 18);
       l2 = text.substring(18);
    } else {
       l1 = text;
    }
  } else {
    l1 = text.substring(0, splitIndex);
    l2 = text.substring(splitIndex + 1);
  }
  l1.trim();
  l2.trim();
}

void fetchLyrics(String track, String artist, String album, uint32_t durationMs) {
  numLyrics = 0;

  xSemaphoreTake(uiMutex, portMAX_DELAY);
  strcpy(spotify.currentLyric1, "Searching lyrics...");
  strcpy(spotify.currentLyric2, "");
  spotify.lyricAlpha = 255;
  isFadingLyric = false;
  xSemaphoreGive(uiMutex);
  forceRedraw = true;

  if (WiFi.status() != WL_CONNECTED) {
    xSemaphoreTake(uiMutex, portMAX_DELAY);
    strcpy(spotify.currentLyric1, "No WiFi");
    strcpy(spotify.currentLyric2, "");
    xSemaphoreGive(uiMutex);
    return;
  }

  String cleanTitle = cleanTrackTitle(track);
  String cleanArtist = cleanArtistName(artist);
  uint32_t durationSec = durationMs / 1000;

  bool isInstrumental = false;
  bool found = false;
  String responseBody;

  // 1. Exact match with duration (if duration between 1 and 3600s per LRCLIB docs)
  if (durationSec > 0 && durationSec <= 3600) {
    String url = "https://lrclib.net/api/get?track_name=" + urlEncode(track) +
                 "&artist_name=" + urlEncode(artist) +
                 "&duration=" + String(durationSec);
    int code = executeLrclibGet(url, responseBody);
    if (code == 200) {
      found = parseGetResult(responseBody, isInstrumental);
    }
  }

  // 2. Exact match without duration
  if (!found && !isInstrumental) {
    delay(200); // 200ms spacing per rate-limit guidelines
    String url = "https://lrclib.net/api/get?track_name=" + urlEncode(track) +
                 "&artist_name=" + urlEncode(artist);
    int code = executeLrclibGet(url, responseBody);
    if (code == 200) {
      found = parseGetResult(responseBody, isInstrumental);
    }
  }

  // 3. Cleaned title match (if different from raw title)
  if (!found && !isInstrumental && (cleanTitle != track || cleanArtist != artist)) {
    delay(200);
    String url = "https://lrclib.net/api/get?track_name=" + urlEncode(cleanTitle) +
                 "&artist_name=" + urlEncode(cleanArtist);
    if (durationSec > 0 && durationSec <= 3600) {
      url += "&duration=" + String(durationSec);
    }
    int code = executeLrclibGet(url, responseBody);
    if (code == 200) {
      found = parseGetResult(responseBody, isInstrumental);
    }
  }

  // 4. Structured search fallback
  if (!found && !isInstrumental) {
    delay(200);
    String searchTitle = (cleanTitle.length() > 0) ? cleanTitle : track;
    String searchArtist = (cleanArtist.length() > 0) ? cleanArtist : artist;
    String url = "https://lrclib.net/api/search?track_name=" + urlEncode(searchTitle) +
                 "&artist_name=" + urlEncode(searchArtist);
    int code = executeLrclibGet(url, responseBody);
    if (code == 200) {
      found = parseSearchResult(responseBody, durationSec, isInstrumental);
    }
  }

  // 5. Query string search fallback (q=)
  if (!found && !isInstrumental) {
    delay(200);
    String q = cleanTitle + " " + cleanArtist;
    q.trim();
    if (q.length() > 0) {
      String url = "https://lrclib.net/api/search?q=" + urlEncode(q);
      int code = executeLrclibGet(url, responseBody);
      if (code == 200) {
        found = parseSearchResult(responseBody, durationSec, isInstrumental);
      }
    }
  }

  // Update UI state with results safely
  xSemaphoreTake(uiMutex, portMAX_DELAY);
  if (isInstrumental) {
    strcpy(spotify.currentLyric1, "♪ Instrumental ♪");
    strcpy(spotify.currentLyric2, "");
  } else if (found && numLyrics > 0) {
    strcpy(spotify.currentLyric1, "♪ Lyrics Synced ♪");
    strcpy(spotify.currentLyric2, "");
  } else {
    strcpy(spotify.currentLyric1, "Lyrics not found");
    strcpy(spotify.currentLyric2, "");
  }
  xSemaphoreGive(uiMutex);
}

void triggerLyricChange(String newText) {
  String l1, l2;
  if (newText.length() > 0) {
    splitLyric(newText, l1, l2);
  } else {
    l1 = "";
    l2 = "";
  }

  xSemaphoreTake(uiMutex, portMAX_DELAY);
  if (String(spotify.currentLyric1) != l1 || String(spotify.currentLyric2) != l2) {
     if (!isFadingLyric) {
        targetLyric1 = l1;
        targetLyric2 = l2;
        isFadingLyric = true;
        fadeDirectionIn = false;
     } else if (fadeDirectionIn) {
        targetLyric1 = l1;
        targetLyric2 = l2;
        fadeDirectionIn = false;
     }
  }
  xSemaphoreGive(uiMutex);
}

int splitWordsHelper(const String& str, String wordsOut[], int maxWords) {
  int count = 0;
  int start = 0;
  int len = str.length();
  while (start < len && count < maxWords) {
    while (start < len && str[start] == ' ') start++;
    if (start >= len) break;
    int end = str.indexOf(' ', start);
    if (end == -1) end = len;
    wordsOut[count++] = str.substring(start, end);
    start = end + 1;
  }
  return count;
}

void computeActiveWord(const String& fullText, uint32_t elapsed, uint32_t duration) {
  String l1, l2;
  splitLyric(fullText, l1, l2);

  String w1[16];
  int count1 = splitWordsHelper(l1, w1, 16);
  String w2[16];
  int count2 = splitWordsHelper(l2, w2, 16);

  int totalWords = count1 + count2;
  if (totalWords == 0 || duration == 0) {
    xSemaphoreTake(uiMutex, portMAX_DELAY);
    spotify.activeWordIdx1 = -1;
    spotify.activeWordIdx2 = -1;
    xSemaphoreGive(uiMutex);
    return;
  }

  // Length-weighted timing (longer words get proportionally more time)
  int weights[32];
  int totalWeight = 0;
  for (int i = 0; i < count1; i++) {
    int w = w1[i].length();
    if (w < 2) w = 2;
    weights[i] = w;
    totalWeight += w;
  }
  for (int i = 0; i < count2; i++) {
    int w = w2[i].length();
    if (w < 2) w = 2;
    weights[count1 + i] = w;
    totalWeight += w;
  }

  // Active singing duration estimation
  uint32_t estSingingMs = totalWords * 380;
  if (estSingingMs < 1200) estSingingMs = 1200;
  uint32_t activeDuration = (duration < estSingingMs) ? duration : estSingingMs;

  int idx1 = -1;
  int idx2 = -1;

  if (elapsed < activeDuration) {
    int currentWeightPos = (elapsed * totalWeight) / activeDuration;
    if (currentWeightPos >= totalWeight) currentWeightPos = totalWeight - 1;

    int activeGlobalIdx = 0;
    int accumWeight = 0;
    for (int i = 0; i < totalWords; i++) {
      accumWeight += weights[i];
      if (currentWeightPos < accumWeight) {
        activeGlobalIdx = i;
        break;
      }
    }

    if (activeGlobalIdx < count1) {
      idx1 = activeGlobalIdx;
      idx2 = -1;
    } else {
      idx1 = -1;
      idx2 = activeGlobalIdx - count1;
    }
  }

  xSemaphoreTake(uiMutex, portMAX_DELAY);
  spotify.activeWordIdx1 = idx1;
  spotify.activeWordIdx2 = idx2;
  xSemaphoreGive(uiMutex);
}

void updateSyncedLyric() {
  if (numLyrics == 0) {
    xSemaphoreTake(uiMutex, portMAX_DELAY);
    spotify.activeWordIdx1 = -1;
    spotify.activeWordIdx2 = -1;
    xSemaphoreGive(uiMutex);
    return;
  }

  // Natural lead time (250ms) for display anticipation
  uint32_t adjMs = spotify.progressMs + 250;

  if (adjMs < lyrics[0].timeMs) {
    triggerLyricChange("");
    xSemaphoreTake(uiMutex, portMAX_DELAY);
    spotify.activeWordIdx1 = -1;
    spotify.activeWordIdx2 = -1;
    xSemaphoreGive(uiMutex);
    return;
  }

  int matchedIndex = -1;
  for (int i = 0; i < numLyrics; i++) {
    if (lyrics[i].timeMs <= adjMs) {
      if (i == numLyrics - 1 || lyrics[i + 1].timeMs > adjMs) {
        matchedIndex = i;
        break;
      }
    }
  }

  if (matchedIndex != -1) {
    String lineText = lyrics[matchedIndex].text;
    triggerLyricChange(lineText);

    if (lineText.length() > 0) {
      uint32_t lineStart = lyrics[matchedIndex].timeMs;
      uint32_t lineEnd = (matchedIndex < numLyrics - 1) ? lyrics[matchedIndex + 1].timeMs : (lineStart + 4000);
      uint32_t lineDuration = (lineEnd > lineStart) ? (lineEnd - lineStart) : 3000;
      if (lineDuration > 6000) lineDuration = 6000;
      if (lineDuration < 800) lineDuration = 800;

      uint32_t elapsed = (adjMs >= lineStart) ? (adjMs - lineStart) : 0;
      computeActiveWord(lineText, elapsed, lineDuration);
    } else {
      xSemaphoreTake(uiMutex, portMAX_DELAY);
      spotify.activeWordIdx1 = -1;
      spotify.activeWordIdx2 = -1;
      xSemaphoreGive(uiMutex);
    }
  }
}

// Spotify Token Logic
void refreshSpotifyToken() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(15000);
  
  HTTPClient http;
  http.begin(client, "https://accounts.spotify.com/api/token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  
  String auth = NVStore::creds.spClientId + ":" + NVStore::creds.spClientSecret;
  http.addHeader("Authorization", "Basic " + base64Encode(auth));

  String payload = "grant_type=refresh_token&refresh_token=" + NVStore::creds.spRefreshToken;
  
  const char * headerKeys[] = {"Retry-After"};
  http.collectHeaders(headerKeys, 1);
  int httpCode = http.POST(payload);

  if (httpCode == 200) {
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, http.getStream());
    if (!error) {
      spotifyAccessToken = doc["access_token"].as<String>();
      tokenExpirationTime = millis() + (doc["expires_in"].as<int>() * 1000) - 60000;
    } else {
      tokenExpirationTime = millis() + 10000; // Backoff on JSON error
    }
  } else if (httpCode == 429) {
    String retryAfterStr = http.header("Retry-After");
    int waitSecs = retryAfterStr.toInt();
    if (waitSecs <= 0) waitSecs = 60; // fallback
    tokenExpirationTime = millis() + (waitSecs * 1000);
    
    String err = "Wait " + String(waitSecs) + "s (429)";
    strncpy(spotify.title, err.c_str(), sizeof(spotify.title)-1);
    spotify.title[sizeof(spotify.title)-1] = '\0';
  } else {
    tokenExpirationTime = millis() + 10000;
  }
  http.end();
}

void fetchAlbumArt(String url) {
  if (url == "") return;
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    int len = http.getSize();
    if (len > 0 && len < 20000) { // Safety check < 20KB for 64x64 jpeg
      uint8_t * buff = (uint8_t *) malloc(len);
      if (buff) {
        WiFiClient * stream = http.getStreamPtr();
        int bytesRead = 0;
        uint32_t startWait = millis();
        while (bytesRead < len && millis() - startWait < 5000) {
          if (stream->available()) {
            bytesRead += stream->read(&buff[bytesRead], len - bytesRead);
            startWait = millis();
          }
          delay(1);
        }
        if (bytesRead == len) {
          TJpgDec.drawJpg(0, 0, buff, len);
        }
        free(buff);
      }
    }
  }
  http.end();
}

void pollSpotifyAPI() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  if (millis() > tokenExpirationTime) {
    refreshSpotifyToken();
  }
  
  if (spotifyAccessToken == "") {
     String err = "No Token (Wait)";
     strncpy(spotify.title, err.c_str(), sizeof(spotify.title)-1);
     spotify.title[sizeof(spotify.title)-1] = '\0';
     return;
  }

  // Track music state for auto-wake
  static bool wasPlaying = false;

  bool pendingFetch = false;
  String pendingTitle = "";
  String pendingArtist = "";
  String pendingAlbum = "";
  uint32_t pendingDurationMs = 0;
  String pendingCoverUrl = "";
  int httpCode = 0;
  
  static uint32_t apiBackoff = 0;
  if (millis() < apiBackoff) {
     return; // Silently wait
  }
  
  {
    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(15000);

    HTTPClient http;
    const char * headerKeys[] = {"Retry-After"};
    http.collectHeaders(headerKeys, 1);
    
    http.begin(client, "https://api.spotify.com/v1/me/player/currently-playing");
    http.addHeader("Authorization", "Bearer " + spotifyAccessToken);
    
    httpCode = http.GET();
  
    if (httpCode == 200) {
      StaticJsonDocument<512> filter;
      filter["is_playing"] = true;
      filter["progress_ms"] = true;
      filter["item"]["name"] = true;
      filter["item"]["artists"][0]["name"] = true;
      filter["item"]["album"]["name"] = true;
      filter["item"]["duration_ms"] = true;
      filter["item"]["album"]["images"][0]["url"] = true;
      filter["item"]["album"]["images"][2]["url"] = true;
      
      DynamicJsonDocument doc(2048);
      DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
      
      xSemaphoreTake(uiMutex, portMAX_DELAY);
      if (error) {
        strcpy(spotify.title, "Parse Error");
        strcpy(spotify.artist, "");
      } else if (doc["is_playing"] == true) {
        spotify.isPlaying = true;
        // Auto-wake screen when music starts playing
        if (!wasPlaying && screenOff) {
          wakeScreen();
        }
        if (doc["item"].isNull()) {
          strcpy(spotify.title, "Ad / Local File");
          strcpy(spotify.artist, "");
        } else {
          String title = doc["item"]["name"].as<String>();
          String artist = doc["item"]["artists"][0]["name"].as<String>();
          String album = doc["item"]["album"]["name"].as<String>();
          
          strncpy(spotify.title, title.c_str(), sizeof(spotify.title)-1);
          spotify.title[sizeof(spotify.title)-1] = '\0';
          strncpy(spotify.artist, artist.c_str(), sizeof(spotify.artist)-1);
          spotify.artist[sizeof(spotify.artist)-1] = '\0';
          
          spotify.progressMs = doc["progress_ms"].as<uint32_t>();
          spotify.durationMs = doc["item"]["duration_ms"].as<uint32_t>();
          
// Sync Interpolation Anchors on Valid Network Metric
          lastKnownProgressMs = spotify.progressMs;
          lastKnownDurationMs = spotify.durationMs;
          lastProgressUpdateMillis = millis();
// 
          
          // Flag for lyric fetch after closing Spotify connection
          if (title != currentTrackForLyrics) {
            pendingTitle = title;
            pendingArtist = artist;
            pendingAlbum = album;
            pendingDurationMs = spotify.durationMs;
            pendingCoverUrl = doc["item"]["album"]["images"][2]["url"].as<String>();
            if (pendingCoverUrl == "null" || pendingCoverUrl.length() < 5) {
               pendingCoverUrl = doc["item"]["album"]["images"][0]["url"].as<String>();
            }
            if (pendingCoverUrl == "null") pendingCoverUrl = ""; // Handle missing art
            pendingFetch = true;
          }
        }
      } else {
        spotify.isPlaying = false;
        strcpy(spotify.title, "Paused");
        strcpy(spotify.artist, "");
        strcpy(spotify.currentLyric1, "");
        strcpy(spotify.currentLyric2, "");
        
        lastKnownDurationMs = 0; 
      }
      xSemaphoreGive(uiMutex);
    } else if (httpCode == 204) {
      xSemaphoreTake(uiMutex, portMAX_DELAY);
      spotify.isPlaying = false;
      strcpy(spotify.title, "Not Playing");
      strcpy(spotify.artist, "");
      strcpy(spotify.currentLyric1, "");
      strcpy(spotify.currentLyric2, "");
      
      lastKnownDurationMs = 0; 
      xSemaphoreGive(uiMutex);
    } else if (httpCode == 429) {
      String retryAfterStr = http.header("Retry-After");
      int waitSecs = retryAfterStr.toInt();
      if (waitSecs <= 0) waitSecs = 60;
      
      apiBackoff = millis() + (waitSecs * 1000); // Backoff exact seconds
      String err = "Wait " + String(waitSecs) + "s (429)";
      xSemaphoreTake(uiMutex, portMAX_DELAY);
      strncpy(spotify.title, err.c_str(), sizeof(spotify.title)-1);
      spotify.title[sizeof(spotify.title)-1] = '\0';
      strcpy(spotify.artist, "Spotify Rate Limit");
      strcpy(spotify.currentLyric1, "");
      strcpy(spotify.currentLyric2, "");
      
      lastKnownDurationMs = 0; 
      xSemaphoreGive(uiMutex);
    } else if (httpCode < 0) {
      apiBackoff = millis() + 3000;
    } else {
      String err = "HTTP Error " + String(httpCode);
      xSemaphoreTake(uiMutex, portMAX_DELAY);
      strncpy(spotify.title, err.c_str(), sizeof(spotify.title)-1);
      spotify.title[sizeof(spotify.title)-1] = '\0';
      strcpy(spotify.artist, "");
      strcpy(spotify.currentLyric1, "");
      strcpy(spotify.currentLyric2, "");
      
      lastKnownDurationMs = 0; 
      xSemaphoreGive(uiMutex);
      apiBackoff = millis() + 5000; 
    }
    
    http.end();
  }
  
  if (pendingFetch) {
    currentTrackForLyrics = pendingTitle;
    
    memset(albumArt, 0, sizeof(albumArt));
    
    fetchAlbumArt(pendingCoverUrl);
    fetchLyrics(pendingTitle, pendingArtist, pendingAlbum, pendingDurationMs);
  }
  
  if (httpCode == 200 && pendingFetch == false) {
    updateSyncedLyric();
  }
  
  // Update music state tracking
  wasPlaying = spotify.isPlaying;

  forceRedraw = true;
}

// FreeRTOS Task (Core 0)
void spotifyTask(void *pvParameters) {
  while (true) {
    pollSpotifyAPI();
    vTaskDelay(3500 / portTICK_PERIOD_MS); 
  }
}