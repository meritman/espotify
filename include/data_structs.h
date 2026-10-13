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
  int16_t  activeWordIdx1; // Active word index in currentLyric1 (-1 for none)
  int16_t  activeWordIdx2; // Active word index in currentLyric2 (-1 for none)
};

