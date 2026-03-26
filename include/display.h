#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include <LovyanGFX.h>
#include <LGFX_ESP32_ST7796S_XPT2046.hpp>

#define TFT_HOR_RES 480
#define TFT_VER_RES 320

extern LGFX tft;
extern uint16_t touchX, touchY, touchZ; // Z:0 = no touch, Z>0 = touching

// Functions
bool initDisplay();
bool initTouch(bool forceCalibrate);
void drawFramebuffer(int x = 0, int y = 0, int w = TFT_HOR_RES, int h = TFT_VER_RES);
void handleTouch();

#endif