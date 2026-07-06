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

// ════════════════════════════════════════════
//  Globals
// ════════════════════════════════════════════
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite sprite = TFT_eSprite(&tft);

#include "ui_volos.h"

SpotifyData spotify   = {};

// --- Smooth progress & lyrics interpolation ---
unsigned long lastProgressUpdateMillis = 0;
int lastKnownProgressMs = 0;
int lastKnownDurationMs = 0;
// ----------------------------------------------

uint32_t lastUpdate    = 0;
bool     forceRedraw   = true;

uint16_t albumArt[15616] = {0}; // 128x122 buffer for background cover art

// --- Screen Timeout ---
uint32_t lastMusicActivityMs = 0;  // tracks when music was last playing
bool     screenOff = false;

// --- Factory Reset Button ---
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
struct LyricLine {
  uint32_t timeMs;
  String text;
};
LyricLine lyrics[100];
int numLyrics = 0;
String currentTrackForLyrics = "";

bool isFadingLyric = false;
bool fadeDirectionIn = false;
String targetLyric1 = "";
String targetLyric2 = "";

// ════════════════════════════════════════════
//  Prototypes
// ════════════════════════════════════════════
void refreshSpotifyToken();
void pollSpotifyAPI();
void fetchLyrics(String track, String artist);
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

// ════════════════════════════════════════════
//  Base64 Helper
// ════════════════════════════════════════════
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

// ════════════════════════════════════════════
//  Screen Timeout
// ════════════════════════════════════════════
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

// ════════════════════════════════════════════
//  Setup — NVS-Driven Boot Flow
// ════════════════════════════════════════════
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

  // ─── Load NVS Credentials ────────────────
  NVStore::load();

  if (!NVStore::hasWiFi()) {
    // ──── PHASE 1: No WiFi → AP Captive Portal ────
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

  // ──── WiFi Credentials Exist → Connect ────
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
    // WiFi failed — fall back to AP mode
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
    // ──── PHASE 2: WiFi OK but no Spotify → Setup Page ────
    tft.setTextColor(0x07E0, TFT_BLACK);
    tft.println("");
    tft.println("Visit to setup:");
    tft.setTextColor(0x07FF, TFT_BLACK);
    tft.println(WiFi.localIP());
    
    Portal::startSTA();
    Serial.println("[Boot] No Spotify creds → serving setup page");
    return;  // Don't start Spotify task yet
  }

  // ──── PHASE 3: All Good → Normal Operation ────
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

// ════════════════════════════════════════════
//  Loop
// ════════════════════════════════════════════
void loop() {
  uint32_t now = millis();

  // ─── Web Server (always runs) ─────────────
  Portal::loop();

  // ─── AP Auto-Shutdown after WiFi connect ──
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

  // ─── Start Spotify task when creds become available ──
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

  // ─── If Spotify not configured, skip rendering ──
  if (!spotifyTaskStarted) {
    return;
  }

  // ─── Factory Reset: Long-Press Button ─────
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

  // --- Screen Timeout (based on music activity) ---
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

  // --- Wake button check ---
  if (screenOff) {
    uint8_t wPin = NVStore::creds.wakePin;
    if (digitalRead(wPin) == LOW) {
      wakeScreen();
    }
  }

  // ─── Spotify UI Rendering ─────────────────
  if (screenOff) return;  // Skip rendering when screen is off
  
  // --- Real-time Spotify Progress Interpolation & Lyric Checking ---
  if (lastKnownDurationMs > 0) {
    int interpolated = lastKnownProgressMs + (now - lastProgressUpdateMillis);
    if (interpolated > lastKnownDurationMs) {
      interpolated = lastKnownDurationMs;
    }
    spotify.progressMs = interpolated;
    
    // Check and trigger line transitions immediately on the interpolated timer
    updateSyncedLyric(); 
  }
  // -----------------------------------------------------------------

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

// ════════════════════════════════════════════
//  LRC Parsing Logic
// ════════════════════════════════════════════
String urlEncode(String str) {
  String encodedString = "";
  encodedString.reserve(str.length() * 3 + 1);
  char c, code0, code1;
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (isalnum(c)) {
      encodedString += c;
    } else {
      code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9) code1 = (c & 0xf) - 10 + 'A';
      c = (c >> 4) & 0xf;
      code0 = c + '0';
      if (c > 9) code0 = c - 10 + 'A';
      encodedString += '%';
      encodedString += code0;
      encodedString += code1;
    }
  }
  return encodedString;
}

void splitLyric(String text, String &l1, String &l2) {
  l1 = ""; l2 = "";
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
}

void fetchLyrics(String track, String artist) {
  numLyrics = 0;
  strcpy(spotify.currentLyric1, "Searching lyrics...");
  strcpy(spotify.currentLyric2, "");
  spotify.lyricAlpha = 255;
  isFadingLyric = false;
  forceRedraw = true;
  
  String encTrack = urlEncode(track);
  String encArtist = urlEncode(artist);
  
  WiFiClientSecure client;
  client.setInsecure(); // Bypass SSL certificate validation to prevent TLS handshake timeouts
  client.setHandshakeTimeout(15000); // Give Cloudflare extra time for TLS handshake
  
  HTTPClient http;
  http.begin(client, "https://lrclib.net/api/get?track_name=" + encTrack + "&artist_name=" + encArtist);
  http.setUserAgent("LrcSyncerESP32/1.0 (https://github.com/shravanpanakkal)");
  http.addHeader("Accept", "application/json");
  http.addHeader("Connection", "close"); // Force server to close connection immediately to prevent -11 timeouts
  http.setTimeout(8000);
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    StaticJsonDocument<200> filter;
    filter["syncedLyrics"] = true;
    
    DynamicJsonDocument doc(8192);
    DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    
    if (error) {
      strcpy(spotify.currentLyric1, "Parse Error");
      strcpy(spotify.currentLyric2, "");
    } else {
      String syncedLyrics = doc["syncedLyrics"].as<String>();
      if (syncedLyrics == "null" || syncedLyrics.length() < 5) {
        strcpy(spotify.currentLyric1, "No Synced Lyrics");
        strcpy(spotify.currentLyric2, "");
        http.end();
        return;
      }
      String lrc = syncedLyrics;
      int pos = 0;
      while (pos < lrc.length() && numLyrics < 100) {
        int nextLine = lrc.indexOf('\n', pos);
        if (nextLine == -1) nextLine = lrc.length();
        
        String line = lrc.substring(pos, nextLine);
        line.trim();
        
        // Parse [MM:SS.xx]
        if (line.length() > 10 && line[0] == '[' && line[3] == ':' && line[6] == '.') {
          uint32_t mins = line.substring(1, 3).toInt();
          uint32_t secs = line.substring(4, 6).toInt();
          uint32_t hund = line.substring(7, 9).toInt();
          
          lyrics[numLyrics].timeMs = (mins * 60000) + (secs * 1000) + (hund * 10);
          
          // Find where the brackets end
          int endBracket = line.indexOf(']');
          if (endBracket != -1 && endBracket < line.length() - 1) {
             String txt = line.substring(endBracket + 1);
             txt.trim();
             lyrics[numLyrics].text = txt;
          } else {
             lyrics[numLyrics].text = "";
          }
          numLyrics++;
        }
        pos = nextLine + 1;
      }
      strcpy(spotify.currentLyric1, "♪ Lyrics Synced ♪");
      strcpy(spotify.currentLyric2, "");
    }
  } else if (httpCode == 404) {
    strcpy(spotify.currentLyric1, "Lyrics not found");
    strcpy(spotify.currentLyric2, "");
  } else {
    sprintf(spotify.currentLyric1, "API Error: %d", httpCode);
    strcpy(spotify.currentLyric2, "");
  }
  http.end();
}

void triggerLyricChange(String newText) {
  String l1, l2;
  splitLyric(newText, l1, l2);
  
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

void updateSyncedLyric() {
  if (numLyrics == 0) return;
  
  // Advance lyrics by 1000ms (1 second earlier)
  uint32_t adjMs = spotify.progressMs + 1000;
  
  int matchedIndex = -1;
  for (int i = 0; i < numLyrics; i++) {
    if (i == 0 && adjMs < lyrics[0].timeMs) {
      triggerLyricChange("");
      return;
    }
    
    if (lyrics[i].timeMs <= adjMs) {
      if (i == numLyrics - 1 || lyrics[i + 1].timeMs > adjMs) {
        matchedIndex = i;
        break;
      }
    }
  }
  
  if (matchedIndex != -1 && lyrics[matchedIndex].text.length() > 0) {
     triggerLyricChange(lyrics[matchedIndex].text);
  }
}

// ════════════════════════════════════════════
//  Spotify API Calls
// ════════════════════════════════════════════
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
          
          strncpy(spotify.title, title.c_str(), sizeof(spotify.title)-1);
          spotify.title[sizeof(spotify.title)-1] = '\0';
          strncpy(spotify.artist, artist.c_str(), sizeof(spotify.artist)-1);
          spotify.artist[sizeof(spotify.artist)-1] = '\0';
          
          spotify.progressMs = doc["progress_ms"].as<uint32_t>();
          spotify.durationMs = doc["item"]["duration_ms"].as<uint32_t>();
          
          // --- Sync Interpolation Anchors on Valid Network Metric ---
          lastKnownProgressMs = spotify.progressMs;
          lastKnownDurationMs = spotify.durationMs;
          lastProgressUpdateMillis = millis();
          // -----------------------------------------------------------
          
          // Flag for lyric fetch after closing Spotify connection
          if (title != currentTrackForLyrics) {
            pendingTitle = title;
            pendingArtist = artist;
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
    fetchLyrics(pendingTitle, pendingArtist);
  }
  
  if (httpCode == 200 && pendingFetch == false) {
    updateSyncedLyric();
  }
  
  // Update music state tracking
  wasPlaying = spotify.isPlaying;

  forceRedraw = true;
}

// ════════════════════════════════════════════
//  FreeRTOS Task (Core 0)
// ════════════════════════════════════════════
void spotifyTask(void *pvParameters) {
  while (true) {
    pollSpotifyAPI();
    vTaskDelay(3500 / portTICK_PERIOD_MS); 
  }
}