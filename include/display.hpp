#ifndef DISPLAY_HPP
#define DISPLAY_HPP

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
/* Replace specified areas with corresponding contents of the framebuffer. 
Passing no parameters, this will be the entire screen. */
void drawFramebuffer(int x = 0, int y = 0, int w = TFT_HOR_RES, int h = TFT_VER_RES);
/** Read from the display, and queue touch points if valid. */
void handleTouch();

#endif