#pragma once
#include <TFT_eSPI.h>
#include "data_structs.h"

// Colors
#define C_BG      TFT_BLACK
#define C_TEXT    TFT_WHITE
#define C_DIM     0x7BEF 
#define C_GRID    0x2104 
#define C_CYAN    0x07FF 
#define C_AMBER   0xFD20 
#define C_GREEN   0x07E0 

extern TFT_eSprite sprite;

// Color Blender for RGB565 Fade Animations
inline uint16_t blendColor(uint16_t fg, uint16_t bg, uint8_t alpha) {
  if (alpha == 255) return fg;
  if (alpha == 0) return bg;
  uint8_t r = ((((fg >> 11) & 0x1F) * alpha) + (((bg >> 11) & 0x1F) * (255 - alpha))) / 255;
  uint8_t g = ((((fg >> 5) & 0x3F) * alpha) + (((bg >> 5) & 0x3F) * (255 - alpha))) / 255;
  uint8_t b = (((fg & 0x1F) * alpha) + ((bg & 0x1F) * (255 - alpha))) / 255;
  return (r << 11) | (g << 5) | b;
}

inline void drawGrid(uint16_t accentColor) {
  sprite.drawFastHLine(0, 24, 128, C_GRID);
  sprite.drawFastHLine(0, 144, 128, C_GRID);
  sprite.fillRect(0, 0, 4, 24, accentColor);
}


// ════════════════════════════════════════════
//  Page 2: Spotify
// ════════════════════════════════════════════
extern uint16_t albumArt[15616];

inline void drawSpotify(const SpotifyData &d) {
  sprite.fillSprite(C_BG);
  
  // Render Pre-scaled 128x120 Album Art Background Instantly
  sprite.pushImage(0, 24, 128, 120, albumArt);

  drawGrid(C_GREEN);
  sprite.setTextDatum(TL_DATUM);
  sprite.setTextFont(2);
  sprite.setTextColor(C_TEXT);
  sprite.drawString("SPOTIFY", 10, 4);
  
// Timestamp (top-right)
if (d.durationMs > 0) {
  int currentSec = d.progressMs / 1000;
  int totalSec   = d.durationMs / 1000;

  char timestamp[20];
  snprintf(timestamp, sizeof(timestamp),
           "%d:%02d|%d:%02d",
           currentSec / 60,
           currentSec % 60,
           totalSec / 60,
           totalSec % 60);

  sprite.setTextDatum(TR_DATUM);
  sprite.setTextFont(1);
  sprite.setTextColor(C_GREEN);
  sprite.drawString(timestamp, 123, 8);

  // Restore the datum for the rest of your code
  sprite.setTextDatum(TL_DATUM);
}

  // Switch to Center Datum for Track Info & Lyrics
  sprite.setTextDatum(TC_DATUM);

  // Track Info (Centered, full width)
  sprite.setTextFont(2);
  sprite.setTextColor(C_TEXT);
  
  int tWidth = sprite.textWidth(d.title);
  if (tWidth > 124) {
    // Scroll animation
    sprite.setTextDatum(TL_DATUM);
    int totalWidth = tWidth + 30; // 30px gap between repeats
    int offset = (millis() / 30) % totalWidth; // 30ms per pixel
    
    // Draw twice for seamless looping
    sprite.drawString(d.title, 4 - offset, 40);
    sprite.drawString(d.title, 4 - offset + totalWidth, 40);
  } else {
    // Normal centered
    sprite.setTextDatum(TC_DATUM);
    sprite.drawString(d.title, 64, 40);
  }

  // Restore datum for the rest of the text!
  sprite.setTextDatum(TC_DATUM);

  sprite.setTextFont(1);
  sprite.setTextColor(C_GREEN);
  sprite.drawString(d.artist, 64, 60);

// Live Lyric (Centered, Faded, up to 2 logical lines, each auto-wraps if long)
  uint16_t lyricColor = blendColor(C_TEXT, C_BG, d.lyricAlpha);
  sprite.setTextColor(lyricColor);

  const int LYRIC_MAX_W = 124;

  // Greedy word-wrap: splits text into rows that fit LYRIC_MAX_W in whatever
  // font is currently set. This replaces the old print()-based auto-wrap,
  // which ignored setTextDatum() and rendered flush-left after the first row.
  auto wrapLyric = [&](const char* text, String rows[], int maxRows) -> int {
    String word, line;
    int count = 0;
    size_t len = strlen(text);

    for (size_t i = 0; i <= len && count < maxRows; i++) {
      char c = (i < len) ? text[i] : ' '; // sentinel: flush last word at the end
      if (c == ' ') {
        if (word.length() == 0) continue;
        String candidate = line.length() ? (line + " " + word) : word;
        if (line.length() > 0 && sprite.textWidth(candidate) > LYRIC_MAX_W) {
          rows[count++] = line;
          line = word;
        } else {
          line = candidate;
        }
        word = "";
      } else {
        word += c;
      }
    }
    if (count < maxRows && line.length() > 0) rows[count++] = line;
    return count;
  };

  // Draws one logical lyric line: largest font that fits on one row, or wraps
  // (centering every row via drawString) if it's still too wide at Font 1.
  // Returns the y just below what it drew, so line 2 can stack under line 1
  // instead of using a fixed offset that could collide with wrapped rows.
  auto drawLyricLine = [&](const char* text, int yPos) -> int {
    if (strlen(text) == 0) return yPos;

    sprite.setTextFont(2);
    if (sprite.textWidth(text) > LYRIC_MAX_W) {
      sprite.setTextFont(1);
    }

    int rowHeight = sprite.fontHeight() + 2;

    if (sprite.textWidth(text) <= LYRIC_MAX_W) {
      sprite.drawString(text, 64, yPos);
      return yPos + rowHeight;
    }

    // Still too wide at Font 1 -> wrap into up to 3 centered rows
    String rows[3];
    int rowCount = wrapLyric(text, rows, 3);
    for (int i = 0; i < rowCount; i++) {
      sprite.drawString(rows[i], 64, yPos + i * rowHeight);
    }
    return yPos + rowCount * rowHeight;
  };

  // Render the layout - line 2 starts wherever line 1 actually ended
  if (d.isPlaying) {
    if (strlen(d.currentLyric2) > 0) {
      int nextY = drawLyricLine(d.currentLyric1, 82);
      drawLyricLine(d.currentLyric2, nextY);
    } else {
      drawLyricLine(d.currentLyric1, 92); // Centered if only one line
    }
  }
  // Progress Bar
  if (d.durationMs > 0) {
    int w = (128 * d.progressMs) / d.durationMs;
    sprite.fillRect(0, 144, w,16, C_GREEN);
  }
}

