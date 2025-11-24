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

// Network
#define LOCAL_HOSTNAME "friendbox"

// Storage
#define TEST_FILE_SIZE (4 * 1024 * 1024)
File testFile;
SPIClass sdspi = SPIClass(HSPI);

/*void handleTouch() {
  uint16_t touchX, touchY;
  static uint16_t color;

  if (tft.getTouch(&touchX, &touchY)) {

    tft.setCursor(400, 5, 2);
    tft.printf("touchX: %i     ", touchX);
    tft.setCursor(400, 50, 2);
    tft.printf("touchY: %i    ", touchY);

    tft.drawPixel(touchX, touchY, color);
    color += 155;
  }
}*/

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
    Serial.println("Card Mount Failed");
    return;
  }
  else
  {
    Serial.println("SD Card initialized.");
  }

  // open the file. note that only one file can be open at a time,
  // so you have to close this one before opening another.
  testFile = SD.open("/test.txt", FILE_WRITE);

  // if the file opened okay, write to it:
  if (testFile)
  {
    Serial.print("Writing to test.txt...");
    testFile.println("testing 1, 2, 3. Fuck ben and ");
    // close the file:
    testFile.close();
    Serial.println("done.");
  }
  else
  {
    // if the file didn't open, print an error:
    Serial.println("error opening test.txt");
  }

  // re-open the file for reading:
  testFile = SD.open("/test.txt");
  if (testFile)
  {
    Serial.println("/test.txt:");

    // read from the file until there's nothing else in it:
    while (testFile.available())
    {
      Serial.write(testFile.read());
    }
    // close the file:
    testFile.close();
  }
  else
  {
    // if the file didn't open, print an error:
    Serial.println("error opening test.txt");
  }
}

void initDisplay()
{
  // Gimme visuals
  Serial.println("Initializing TFT_eSPI...");
  tft.init();
  tft.fillScreen(TFT_BLACK);
  tft.setRotation(3);

  // Gimme text
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(0, 20, 2);
  tft.setTextSize(2);
  tft.println("Welcome to FriendBox.");
  tft.println("Please wait...");

  // Gimme touch
  uint16_t calibrationData[5];
  uint8_t calDataOK = 0;
  // check file system
  // Format regardless.
  // SPIFFS.format();
  if (!SPIFFS.begin())
  {
    Serial.println("No SPIFFS exists. Formatting file system...");
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
  // handleTouch();
  // Serial.println(tft.readPixel(100, 100));
}

/*use Arduinos millis() as tick source*/
static uint32_t systemTick(void)
{
  return millis();
}
