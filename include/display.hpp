#ifndef DISPLAY_HPP
#define DISPLAY_HPP

#include <Arduino.h>
#include <LovyanGFX.h>
#include <LGFX_ESP32_PCBA5981_GT911.hpp>

#define TFT_HOR_RES 480
#define TFT_VER_RES 480

extern LGFX tft;
extern uint16_t touchX, touchY, touchZ; // Z:0 = no touch, Z>0 = touching

// Functions
bool initDisplay();
/** Read touch input as it becomes available and write to global variables.*/
void handleTouch();

#endif