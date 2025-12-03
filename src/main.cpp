#include "FS.h"
#include <WiFi.h>
#include <SPI.h>
#include <SPIFFS.h>
#include <SD.h>
#include <LovyanGFX.hpp>
#include <LGFX_ESP32_ST7796S_XPT2046.hpp>
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
/** How many "inputs" should we drop after intial touch and liftoff? */
#define TOUCH_INPUT_BUFFER 10
/** How many times we have registered a touch input. */
static uint16_t touch_count = 0;
/** Touch inputs waiting to be drawn (insane asylum) */
static uint16_t touch_queue_x[TOUCH_INPUT_BUFFER];
static uint16_t touch_queue_y[TOUCH_INPUT_BUFFER];
static uint8_t touch_queue_lastwrite_position = 0;

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
void drawBrushToFB(int x, int y, int radius, uint8_t colorIndex);
void handleTouch();
void handleTouchQueue();

void setup()
{
#ifdef FRIENDBOX_DEBUG_MODE
  Serial.begin(115200);
#endif
  initDisplay();
  initSD(false);
  initTouch(false);
  // initNetwork();
}

void loop()
{
  handleTouch();
}

/** Read from the display, and queue touch points if valid. */
void handleTouch()
{
  uint16_t touchX, touchY;
  if (lcd.getTouch(&touchX, &touchY))
  { // Touching
    if (touchX >= 0 && touchX < lcd.width() &&
        touchY >= 0 && touchY < lcd.height())
    {
      // Serial.print("Touch - X: ");
      // Serial.print(touchX);
      // Serial.print(" Y: ");
      // Serial.println(touchY);

      if (++touch_count > TOUCH_INPUT_BUFFER)
      {
        if (touch_queue_lastwrite_position < TOUCH_INPUT_BUFFER)
        {
          touch_queue_x[touch_queue_lastwrite_position] = touchX;
          touch_queue_y[touch_queue_lastwrite_position] = touchY;
          
          touch_queue_lastwrite_position =
          (touch_queue_lastwrite_position + 1) % TOUCH_INPUT_BUFFER;
        }
        handleTouchQueue(); // Move thru queue retroactively, but only if touching.
      }
    }
  }
  else
  { // No longer touching, re-init to zero.
    touch_count = 0;
    memset(touch_queue_x, 0, sizeof(touch_queue_x));
    memset(touch_queue_y, 0, sizeof(touch_queue_y));
  }
}

/** Process touch points from queue if valid. */
void handleTouchQueue()
{
  uint8_t touch_queue_read_position = (touch_queue_lastwrite_position + TOUCH_INPUT_BUFFER - (TOUCH_INPUT_BUFFER - 1)) % TOUCH_INPUT_BUFFER;

  //Serial.print("Last Write Pos: ");
  //Serial.println(touch_queue_lastwrite_position);
  //Serial.print("Read Pos: ");
  //Serial.println(touch_queue_read_position);
  //Serial.print("X Value: ");
  //Serial.println(touch_queue_x[touch_queue_read_position]);

  if (((touch_queue_x[touch_queue_read_position] + 
      touch_queue_y[touch_queue_read_position]) > 0)) {
    drawBrushToFB(touch_queue_x[touch_queue_read_position], 
      touch_queue_y[touch_queue_read_position], 3, 3);
    //Serial.println("Drawing.");
  }
}

void drawBrushToFB(int x, int y, int radius, uint8_t colorIndex)
{
  // Draw filled circle using midpoint circle algorithm
  for (int dy = -radius; dy <= radius; dy++)
  {
    for (int dx = -radius; dx <= radius; dx++)
    {
      // Check if point is inside circle
      if (dx * dx + dy * dy <= radius * radius)
      {
        int px = x + dx;
        int py = y + dy;

        // Draw to framebuffer
        // drawPixelToFB(px, py, colorIndex);

        // Draw to screen immediately for instant feedback
        if (px >= 0 && px < lcd.width() && py >= 0 && py < lcd.height())
        {
          lcd.drawPixel(px, py, draw_color_palette[colorIndex]);
        }
      }
    }
  }
}

/** Initialize / Reconnect Network. Credentials are hardcoded for now, but will program to store on SD card. */
void initNetwork()
{

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
#ifdef FRIENDBOX_DEBUG_MODE
  Serial.println("INFO: Initializing LGFX touch...");
#endif
  uint16_t calibration_data[8];
  bool calibration_data_ok = false;

  File touch_calibration_file = SD.open("/touch_calibration_file.bin", FILE_READ);
  if (touch_calibration_file)
  { // File present, read and apply.
    if (touch_calibration_file.readBytes((char *)calibration_data, 16) == 16)
    {
      calibration_data_ok = true;
#ifdef FRIENDBOX_DEBUG_MODE
      Serial.println("INFO: Calibration Data OK!");
#endif
    }
    else
    {
#ifdef FRIENDBOX_DEBUG_MODE
      Serial.println("INFO: Calibration Data is incomplete or corrupted! Deleting...");
#endif
      SD.remove("/touch_calibration_file.bin");
    }
    touch_calibration_file.close();
  }

  if (!calibration_data_ok || forceCalibrate)
  { // data not valid. recalibrate
#ifdef FRIENDBOX_DEBUG_MODE
    Serial.println("INFO: Recreating touchscreen calibration because:");
    Serial.print("calibration_data_ok: ");
    Serial.println(calibration_data_ok);
    Serial.print("forceCalibrate: ");
    Serial.println(forceCalibrate);
#endif
    lcd.fillScreen(TFT_BLACK);
    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setCursor(160, 40);
    lcd.setTextSize(4);
    lcd.drawCenterString("Touch Calibration", 240, 70);
    lcd.setTextSize(3);
    lcd.drawCenterString("Press Highlighted Corners...", 240, 120);

    lcd.calibrateTouch(calibration_data, TFT_WHITE, TFT_RED, 15);

#ifdef FRIENDBOX_DEBUG_MODE
    Serial.println("Touch Calibration Data");
    for (int i = 0; i < 8; i++)
    {
      Serial.println(calibration_data[i]);
    }
#endif
    File touch_calibration_file = SD.open("/touch_calibration_file.bin", FILE_WRITE);
    if (touch_calibration_file)
    {
      touch_calibration_file.write((const unsigned char *)calibration_data, sizeof(calibration_data));
      touch_calibration_file.close();
#ifdef FRIENDBOX_DEBUG_MODE
      Serial.println("INFO: Successfully wrote calibration data to SD.");
#endif
    }
  }

  lcd.setTouchCalibrate(calibration_data);
}

void initSD(bool forceFormat)
{
#ifdef FRIENDBOX_DEBUG_MODE
  Serial.println("INFO: Initializing SD...");
#endif
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
