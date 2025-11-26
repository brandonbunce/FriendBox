#include "FS.h"
#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <SD.h>
#include "secrets.h"

// Touch
#define CALIBRATION_FILE "/calibrationData"

// Display
TFT_eSPI tft = TFT_eSPI();
uint8_t* fb4;

// Network
#define LOCAL_HOSTNAME "friendbox"

// Storage
#define TEST_FILE_SIZE (4 * 1024 * 1024)
File displayFile;
SPIClass sdspi = SPIClass(HSPI);
String serialCommand;

// Functions
void updateDisplayWithFB();
void drawPixelToFB(int x, int y, uint8_t colorIndex);

void handleTouch() {
  uint16_t touchX, touchY;
  static uint16_t color;

  if (tft.getTouch(&touchX, &touchY)) {
    Serial.print("Touch - X: ");
    Serial.print(touchX);
    Serial.print(" Y: ");
    Serial.println(touchY);
    
    // Touch coordinates already match screen/framebuffer!
    drawPixelToFB(touchX, touchY, 2);
    tft.drawPixel(touchX, touchY, color);
    color += 155;
  }
}

void initNetwork(const char *netSSID, const char *netPassword, const char *hostname)
{
  tft.print("Connecting to ");
  tft.println(netSSID);
  WiFi.setHostname(hostname);
  WiFi.begin(netSSID, netPassword);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    tft.print(".");
  }
  tft.println("");
  tft.print("Connected to ");
  tft.println(netSSID);
  tft.print("IP Address: ");
  tft.print(WiFi.localIP());
  // What if the network cannot ever connect? How do we handle?
}

void initSD(int pin)
{

  sdspi.begin(14 /* SCK */, 32 /* MISO */, 13 /* MOSI */, 26 /* SS */);

  if (!SD.begin(pin, sdspi))
  {
    Serial.println("ERROR: Card mount failed!");
    return;
  }
  else
  {
    Serial.println("INFO: SD card is ready.");
  }
}

void initDisplay()
{
  // Initialize display
  Serial.println("Initializing TFT_eSPI...");
  tft.init();
  tft.fillScreen(TFT_BLACK);
  tft.setRotation(3);

  // Console
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(0, 20, 2);
  tft.setTextSize(2);
  tft.println("Welcome to FriendBox.");
  tft.println("Please wait...");

  // Initialize touch
  uint16_t calibrationData[5];
  uint8_t calDataOK = 0;
  // check file system
  // Format regardless.
  // SPIFFS.format();
  if (!SPIFFS.begin())
  {
    Serial.println("INFO: No SPIFFS exists. Formatting file system...");
    SPIFFS.format();
    SPIFFS.begin();
  }

  // check if calibration file exists
  if (SPIFFS.exists(CALIBRATION_FILE))
  {
    File f = SPIFFS.open(CALIBRATION_FILE, "r");
    if (f)
    {
      if (f.readBytes((char *)calibrationData, 14) == 14)
        calDataOK = 1;
      f.close();
    }
  }
  if (calDataOK)
  {
    // calibration data valid
    tft.setTouch(calibrationData);
    Serial.println("Touch calibration data valid. Proceeding.");
  }
  else
  {
    // data not valid. recalibrate
    tft.calibrateTouch(calibrationData, TFT_WHITE, TFT_RED, 15);
    // store data
    File f = SPIFFS.open(CALIBRATION_FILE, "w");
    if (f)
    {
      f.write((const unsigned char *)calibrationData, 14);
      f.close();
    }
  }
  
  // Allocate framebuffer in ROTATED dimensions
  fb4 = (uint8_t*) malloc((tft.width() * tft.height()) / 2); // 76.8 KB
  if (!fb4) {
    Serial.println("FATAL: Framebuffer allocation failed!");
    while(1);
  }
  memset(fb4, 0, (tft.width() * tft.height()) / 2);
}

void drawPixelToFB(int x, int y, uint8_t colorIndex)
{
    if (x < 0 || x >= tft.width() || y < 0 || y >= tft.height()) {
      Serial.println("WARN: Pixel out of bounds.");
      return;
    }

    int index = y * tft.width() + x;
    int byteIndex = index >> 1;

    if (index & 1)
        fb4[byteIndex] = (fb4[byteIndex] & 0xF0) | (colorIndex & 0x0F);
    else
        fb4[byteIndex] = (fb4[byteIndex] & 0x0F) | ((colorIndex & 0x0F) << 4);
}

void drawTest4() {
  for (int y = 0; y < tft.height(); y++) {
    for (int x = 0; x < tft.width(); x++) {
      uint8_t color = (x >> 5) & 0x0F; // 16 vertical stripes
      drawPixelToFB(x, y, color);
    }
  }
}

void saveImageToSD() {
  File f = SD.open("/fb4.bin", FILE_WRITE);
  if (f) {
    f.write(fb4, (tft.width() * tft.height()) / 2);
    f.close();
    Serial.println("Image saved!");
  }
}

void loadImageFromSD() {
  File f = SD.open("/fb4.bin", FILE_READ);
  if (f) {
    f.read(fb4, (tft.width() * tft.height()) / 2);
    f.close();
    updateDisplayWithFB();
    Serial.println("Image loaded!");
  }
}

uint16_t palette[16] = {
  0x0000, // black
  0xFFFF, // white
  0xF800, // red
  0x07E0, // green
  0x001F, // blue
  0xFFE0, // yellow
  0xF81F, // magenta
  0x07FF, // cyan
  0x8410, // gray
  0x4208, // dark gray
  0xFC10, // orange
  0x07F0, // aqua
  0x780F, // purple
  0x03EF, // teal
  0xF810, // red-ish
  0xFFFF  // white duplicate
};

/** Wipe entire screen and replace with contents of the 4-bit color framebuffer. */
void updateDisplayWithFB() {
  tft.startWrite();
  tft.setAddrWindow(0, 0, tft.width(), tft.height());

  for (int i = 0; i < (tft.width() * tft.height()) / 2; i++) {
    uint8_t b = fb4[i];
    tft.pushColor(palette[b >> 4]);
    tft.pushColor(palette[b & 0x0F]);
  }

  tft.endWrite();
}

void handleSerialCommand() {
  if (Serial.available()) {
    serialCommand = Serial.readStringUntil('\n');
    serialCommand.toLowerCase();
    serialCommand.trim();
    if (serialCommand.equals("drawpattern")) 
    {
      drawTest4();
      updateDisplayWithFB();
    }
    else if (serialCommand.equals("save")) 
    {
      saveImageToSD();
    }
    else if (serialCommand.equals("load")) 
    {
      loadImageFromSD();
    }
    else if (serialCommand.equals("speed")) 
    {
      int argument1;
      Serial.println("Please select fan speed (1-6):");
      while (!argument1) 
      {
        argument1 = Serial.readStringUntil('\n').toInt();
      }
      Serial.print("Selected fan speed: ");
      Serial.println(String(argument1));
    } 
    else 
    {
      Serial.println("Invalid command!");
    }
  }
}

void setup()
{
  // cawkins was here
  Serial.begin(115200);
  initDisplay();
  initSD(26);
  // initNetwork(NETWORK_SSID, NETWORK_PASS, LOCAL_HOSTNAME);
}

void loop()
{
  handleTouch();
  handleSerialCommand();
}
