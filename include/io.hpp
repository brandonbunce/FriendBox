#ifndef IO_H
#define IO_H

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>

#define SD_CS 12
#define SD_SCK 16
#define SD_MISO 21
#define SD_MOSI 15
extern Preferences nvs;

// Input (Buttons)
/** Which GPIO pin will be used as input for the hall effect button? */
#define HALL_SENSOR_PIN 15
/** How long should button be pressed before logically registering input? */
#define DEBOUNCE_MILLISECONDS 50

// Functions
bool initSD(bool forceFormat = false);
bool initNVS();
/* Prepare ESP32 to receive inputs from button / hall effect. */
bool initMenuButton();
/**
 * Handle pressing of hardware button, will implement as hall effect sensor later
 * recheckInput will register another logical press even if button is being held.
 * @param recheckInput Should we register another input in the event the button is still being held?
 */
void handleMenuButton(bool recheckInput);
void saveImageToSD(int slot);
/** Load a sketch from SD given it's file name. */
void loadSketchFromSD(const char *path);
void loadImageFromSD(int slot);
std::vector<std::string> sdGetFboxFiles();

#endif