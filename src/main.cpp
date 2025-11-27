#include "FS.h"
#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <SD.h>
#include "secrets.h"

// Touch
#define CALIBRATION_FILE "/calibrationData"
TFT_eSPI_Button colorButton[15];

// Input
#define MENU_BUTTON_PIN 0
static unsigned long lastPress = 0;
static unsigned int lastButtonState = 0;
static bool alreadyPressed = false;
#define DEBOUNCE_MILLISECONDS 50
// Invoke the TFT_eSPI button class and create all the button objects

// Output

// Display
TFT_eSPI tft = TFT_eSPI();
uint8_t* fb4;
typedef enum {
  TOOL_ERASER,  
  TOOL_BRUSH,
  TOOL_PENCIL,
  TOOL_DITHER,
  TOOL_FILL
} draw_tool_id_t;
/** Defines color palette for our 4-bit color frame buffer.
 * Color palette is from https://androidarts.com/palette/16pal.htm
*/
uint16_t palette[16] = {
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
static int currentBackgroundColorIndex = 0;
static int currentDrawColorIndex = 11;
static int currentBrushRadius = 5;
#define DRAW_MENU_TOP_BAR_HEIGHT_PX 50
#define DRAW_MENU_BOTTOM_BAR_HEIGHT_PX 25
#define DRAW_MENU_BOTTOM_BAR_ITEM_WIDTH_PX 27
static bool menuBarOpen = false;

// Network
#define LOCAL_HOSTNAME "friendbox"

// Storage
#define TEST_FILE_SIZE (4 * 1024 * 1024)
File displayFile;
SPIClass sdspi = SPIClass(HSPI);
String serialCommand;

// Functions
void updateDisplayWithFB();
void calibrateDisplay(bool force);
void setDrawColor(uint8_t colorIndex);
void drawPixelToFB(int x, int y, uint8_t colorIndex);
void drawBrushToFB(int x, int y, int radius, uint8_t colorIndex);
void drawingMenu(bool show);

void handleTouch() {
  uint16_t touchX, touchY;
  static uint16_t color;

  if (tft.getTouch(&touchX, &touchY)) {
    Serial.print("Touch - X: ");
    Serial.print(touchX);
    Serial.print(" Y: ");
    Serial.println(touchY);
    //Serial.print(" Z: ");
    //Serial.println(tft.getTouchRawZ());
    
    // Touch coordinates already match screen/framebuffer!
    //drawPixelToFB(touchX, touchY, 2);
    if (menuBarOpen) {
      if ((touchY > DRAW_MENU_TOP_BAR_HEIGHT_PX) &&
       (touchY < (tft.height() - DRAW_MENU_BOTTOM_BAR_HEIGHT_PX))) {
          drawBrushToFB(touchX, touchY, currentBrushRadius, currentDrawColorIndex);
       }
       else {
        for (int button = 0; button < 15; button++) {
          if (colorButton[button].contains(touchX, touchY)) {
            colorButton[button].press(true);
          }
          else {
            colorButton[button].press(false);
          }
        }
       }
    }
    else {
      drawBrushToFB(touchX, touchY, currentBrushRadius, currentDrawColorIndex);
    }
  }
}

void handleTouchButtonUpdate() {
  for (uint8_t b = 0; b < 15; b++) {

    if (colorButton[b].justReleased() && menuBarOpen) colorButton[b].drawButton();     // draw normal

    if (colorButton[b].justPressed() && menuBarOpen) {
      colorButton[b].drawButton(true);  // draw invert

      Serial.print("Touch color");
      Serial.println(b);
      setDrawColor(b);
    }
  }
}

/** This is pretty much useless as it doesn't debounce release, so it will probably
 * just break in the future. Rewrite when necessary :)) Also not sure if this handles
 * rollover correctly.
 */
void handleMenuButton() {
  if (digitalRead(MENU_BUTTON_PIN) == LOW) /*Button Pressed*/ {
    if (lastPress == 0) {
      lastPress = millis();
    }
    if ((millis() >= (lastPress + DEBOUNCE_MILLISECONDS)) & !alreadyPressed)
    {
      Serial.println("Logical Press");
      drawingMenu(true);
      alreadyPressed = true;
    }
    else {return;}
  }
  else /*Button Released*/{
    if (lastPress) {
      lastPress = 0;
      alreadyPressed = false;
      Serial.println("Logical Release.");
      drawingMenu(false);
    }
  }
}

void drawingMenu(bool show) {
  if (show) {
    menuBarOpen = true;
    //tft.fillRect(0, 0, 480, 50, TFT_WHITE);
    //tft.fillRect(0, 270, 480, 50, TFT_WHITE);
    for (int col = 0; col < 16; col++){
      Serial.print(col);
      colorButton[col].initButtonUL(&tft, (col*30), (tft.height() - DRAW_MENU_BOTTOM_BAR_HEIGHT_PX), 
      DRAW_MENU_BOTTOM_BAR_ITEM_WIDTH_PX, DRAW_MENU_BOTTOM_BAR_HEIGHT_PX, 
      TFT_WHITE, palette[col], palette[1], "", 1);
      colorButton[col].drawButton();
    }
  }
  else {
    menuBarOpen = false;
    updateDisplayWithFB();
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

  // Calibrate Touch
  calibrateDisplay(false);
  
  // Allocate framebuffer in ROTATED dimensions
  fb4 = (uint8_t*) malloc((tft.width() * tft.height()) / 2); // 76.8 KB
  if (!fb4) {
    Serial.println("FATAL: Framebuffer allocation failed!");
    while(1);
  }
  memset(fb4, 0, (tft.width() * tft.height()) / 2);
}

void calibrateDisplay(bool force) {
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
  if (calDataOK && !force)
  {
    // calibration data valid
    tft.setTouch(calibrationData);
    Serial.println("Touch calibration data valid. Proceeding.");
  }
  else
  {
    // data not valid. recalibrate
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(160, 40);
    tft.setTextSize(3);
    tft.drawCentreString("Touch Calibration", 240, 70, 2);
    tft.setTextSize(2);
    tft.drawCentreString("Press Highlighted Corners...", 240, 120, 2);

    tft.calibrateTouch(calibrationData, TFT_WHITE, TFT_RED, 15);
    // store data
    File f = SPIFFS.open(CALIBRATION_FILE, "w");
    if (f)
    {
      f.write((const unsigned char *)calibrationData, 14);
      f.close();
    }
    tft.fillScreen(TFT_BLACK);
    Serial.println("Touch recalibration successful.");
  }
}

void drawPixelToFB(int x, int y, uint8_t colorIndex)
{
    if (x < 0 || x >= tft.width() || y < 0 || y >= tft.height()) return;

    int index = y * tft.width() + x;
    int byteIndex = index >> 1;

    if (index & 1)
        fb4[byteIndex] = (fb4[byteIndex] & 0xF0) | (colorIndex & 0x0F);
    else
        fb4[byteIndex] = (fb4[byteIndex] & 0x0F) | ((colorIndex & 0x0F) << 4);
}

// Helper functions to change tool settings
void setDrawColor(uint8_t colorIndex) {
  if (colorIndex < 16) {
    Serial.print("Color set to: ");
    Serial.println(colorIndex);
    currentDrawColorIndex = colorIndex;
  }
}

void setBackgroundColor(uint8_t colorIndex) {
  if (colorIndex < 16) {
    Serial.print("Background color set to: ");
    Serial.println(colorIndex);
    currentBackgroundColorIndex = colorIndex;
    tft.fillScreen(palette[colorIndex]);
  }
}

void setBrushRadius(int radius) {
  if (radius > 0 && radius <= 50) {
    Serial.print("Brush radius set to: ");
    Serial.println(radius);
  }
}

// Draw a circle brush at x,y with given radius and color
// Updates BOTH framebuffer and screen in real-time!
void drawBrushToFB(int x, int y, int radius, uint8_t colorIndex) {
  // Draw filled circle using midpoint circle algorithm
  for (int dy = -radius; dy <= radius; dy++) {
    for (int dx = -radius; dx <= radius; dx++) {
      // Check if point is inside circle
      if (dx * dx + dy * dy <= radius * radius) {
        int px = x + dx;
        int py = y + dy;
        
        // Draw to framebuffer
        drawPixelToFB(px, py, colorIndex);
        
        // Draw to screen immediately for instant feedback
        if (px >= 0 && px < tft.width() && py >= 0 && py < tft.height()) {
          tft.drawPixel(px, py, palette[colorIndex]);
        }
      }
    }
  }
}

void drawToolToFB(int radius, draw_tool_id_t tool, uint8_t colorIndex) {

  switch (tool) 
  {
    case TOOL_BRUSH: 
      break;
    default:
      Serial.println("WARN: Trying to draw with an invalid / not implemented tool.");
      break;
  }
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
    else if (serialCommand.equals("calibrate")) 
    {
      calibrateDisplay(true);
    }
    else if (serialCommand.equals("setcolor")) 
    {
      int argument1 = NULL;
      Serial.println("Please select fan speed (1-6):");
      while (!argument1) 
      {
        argument1 = Serial.readStringUntil('\n').toInt();
      }
      Serial.print("Selected fan speed: ");
      Serial.println(String(argument1));
      setDrawColor(argument1);
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
  pinMode(MENU_BUTTON_PIN, INPUT);
}

void loop()
{
  handleTouch();
  handleTouchButtonUpdate();
  handleSerialCommand();
  // We just gotta run this on loop until we can set up interrupts.
  handleMenuButton();
}
