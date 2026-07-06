#pragma once

// ═══════════════════════════════════════════
//  Hardware Pin Map
// ═══════════════════════════════════════════
//  MOSI  → 23    SCK  → 18    CS  → 5
//  DC/A0 → 2     RST  → 4
//  BL    → 22 (PWM)   BTN → 12 (INPUT_PULLUP)
// ═══════════════════════════════════════════

#define PIN_BL       22       // Backlight (LEDC PWM)

// ─── Display ────────────────────────────────
#define SCREEN_W    128       // Portrait width
#define SCREEN_H    160       // Portrait height
#define SCREEN_ROT    2       // Portrait rotation (180 deg)

// ─── Backlight PWM ──────────────────────────
#define BL_CHANNEL    0       // LEDC channel (core <3.x)
#define BL_FREQ    5000       // Hz
#define BL_RES        8       // 8-bit (0-255)
#define BL_FULL     255
#define BL_DIM       40
#define BL_OFF        0

// ─── Timing ─────────────────────────────────
#define UPDATE_INTERVAL  1000  // Data refresh ms


