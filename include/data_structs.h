#pragma once
#include <Arduino.h>

struct SpotifyData {
  char     title[64];
  char     artist[64];
  char     currentLyric1[64];
  char     currentLyric2[64];
  uint8_t  lyricAlpha;
  uint32_t progressMs;
  uint32_t durationMs;
  bool     isPlaying;
};

