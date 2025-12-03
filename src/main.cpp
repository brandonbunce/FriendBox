#include "FS.h"
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <LovyanGFX.hpp>
#include <lgfx_user/LGFX_ESP32_ST7796S_XPT2046.h>
#include <lvgl.h>
#include "secrets.h"

// (C) 2025-2026 Brandon Bunce - FriendBox System Software
#define FRIENDBOX_DEBUG_MODE true
#define FRIENDBOX_SOFTWARE_VERSION "Software v0.1"

// Display
static LGFX lcd;
uint16_t draw_color_palette[16] = {
    0x0000, // Black (0)
    0x9CF3, // Gray (1)
    0xFFFF, // White (2)
    0xb926, // Red (3)
    0xdb71, // Meat (4)
    0x49e5, // Dark Brown (5)
    0xa324, // Brown (6)
    0xec46, // Orange (7)
    0xf70d, // Yellow (8)
    0x3249, // Dark Green (9)
    0x4443, // Green (10)
    0xa665, // Slime Green (11)
    0x1926, // Night Blue (12)
    0x02b0, // Sea Blue (13)
    0x351d, // Sky Blue (14)
    0xb6dd  // Cloud Blue (15)
};

// Touch

// Input
String serialCommand;

// Output

// Storage
SPIClass sdspi = SPIClass(HSPI);
#define SD_CS 26
#define SD_SCK 14
#define SD_MISO 32
#define SD_MOSI 13

// Network
#define LOCAL_HOSTNAME "friendbox"

// Function Declarations
void setup();
void loop();
void initDisplay();
void initTouch(bool forceCalibrate);
void initSD(bool forceFormat);
void initNetwork();

void setup()
{
#ifdef FRIENDBOX_DEBUG_MODE
  Serial.begin(115200);
#endif
  initDisplay();
  initSD(false);
  //initNetwork();
}

void loop()
{
}

/** Initialize / Reconnect Network. Credentials are hardcoded for now, but will program to store on SD card. */
void initNetwork() {
  
  WiFi.setHostname(LOCAL_HOSTNAME);
  WiFi.begin(NETWORK_SSID, NETWORK_PASS);
  #ifdef FRIENDBOX_DEBUG_MODE
  Serial.println("INFO: Connecting to WiFi.");
  #endif
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    #ifdef FRIENDBOX_DEBUG_MODE
    Serial.print(".");
    #endif
  }
  #ifdef FRIENDBOX_DEBUG_MODE
  Serial.print("INFO: Connected to ");
  Serial.println(NETWORK_SSID);
  Serial.print("INFO: IP Address: ");
  Serial.print(WiFi.localIP());
  #endif
  // What if the network cannot ever connect? How do we handle?
}

void initDisplay()
{
  #ifdef FRIENDBOX_DEBUG_MODE
    Serial.println("INFO: Initializing LGFX...");
  #endif
  lcd.init();
  lcd.setRotation(3); // This option enables suffering. Don't forget to account for coordinate translation!
  lcd.setBrightness(255);
  lcd.setColorDepth(16);
  lcd.fillScreen(TFT_DARKCYAN);
  lcd.setTextColor(TFT_GOLD, TFT_DARKCYAN);
  lcd.setTextSize(4);
  lcd.drawCenterString("FriendBox", 240, 120);
  lcd.setTextSize(3);
  lcd.drawCenterString(FRIENDBOX_SOFTWARE_VERSION, 240, 160);
}

void initTouch(bool forceCalibrate)
{
}

void initSD(bool forceFormat)
{
  sdspi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS, sdspi))
  {
    #ifdef FRIENDBOX_DEBUG_MODE
    Serial.println("ERROR: SD mount failed! Is it connected properly?");
    #endif
    // Todo - Show on-screen error.
    return;
  }
  else
  {
    #ifdef FRIENDBOX_DEBUG_MODE
    Serial.println("INFO: SD ready!");
    #endif
  }
}
