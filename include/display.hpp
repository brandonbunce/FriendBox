#ifndef DISPLAY_HPP
#define DISPLAY_HPP

#include <Arduino.h>
#include <LovyanGFX.h>
#include <LGFX_ESP32_PCBA5981_GT911.hpp>

#define TFT_HOR_RES 480
#define TFT_VER_RES 480

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