#ifndef DISPLAY_HPP
#define DISPLAY_HPP

#include <Arduino.h>
#include <LovyanGFX.h>
#include <LGFX_ESP32_PCBA5981_GT911.hpp>

#define TFT_HOR_RES 480
#define TFT_VER_RES 480

// LT7680 SDRAM frame slot addresses. The LT7680 scans/draws at native 16bpp,
// so each full-screen slot is TFT_HOR_RES * TFT_VER_RES * 2 bytes.
//   SLOT_CANVAS — canonical drawing surface (the user's sketch).
//   SLOT_UI     — overlay backing store; CANVAS_MENU snapshots SLOT_CANVAS
//                 here, then paints UI buttons over the snapshot. Switching
//                 the panel's main-image address back to SLOT_CANVAS makes
//                 the overlay disappear instantly with no fillRect needed.
// Both addresses must be 4-byte aligned (TFT_HOR_RES*2 already is).
#define LT7680_FRAME_BYTES ((uint32_t)TFT_HOR_RES * TFT_VER_RES * 2)
#define LT7680_SLOT_CANVAS 0u
#define LT7680_SLOT_UI     LT7680_FRAME_BYTES

extern LGFX tft;
/* Tracks touch location in current tick- If Z>0 then touching. */
extern uint16_t touchX, touchY, touchZ;
/* Tracks touch location in previous tick. Does not reset when releasing
touch, so check touchZ to avoid stale numbers.*/
extern u_int16_t lastTouchX, lastTouchY;

// Functions
bool initDisplay();
/** Read touch input as it becomes available and write to global variables.*/
void handleTouch();

#endif