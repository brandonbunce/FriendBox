#ifndef IO_H
#define IO_H

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>

extern Preferences nvs;

// Functions
bool initSD(bool forceFormat = false);
bool initNVS();
bool initMenuButton();
void handleMenuButton(bool recheckInput);
void saveImageToSD(int slot);
void loadSketchFromSD(const char *path);
void loadImageFromSD(int slot);
std::vector<std::string> sdGetFboxFiles();

#endif