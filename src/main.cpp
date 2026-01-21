#include "FS.h"
#include <Arduino.h>
#include <WiFi.h>
#include <LovyanGFX.h>
#include <LGFX_ESP32_ST7796S_XPT2046.hpp>
#include <SPI.h>
#include <SD.h>
#include "secrets.h"

// (C) 2025-2026 Brandon Bunce - FriendBox System Software
#define FRIENDBOX_DEBUG_MODE true
#define FRIENDBOX_SOFTWARE_VERSION "Software v0.1 (NO-LVGL)"

// Touch
uint16_t touchX, touchY, touchZ; // Z:0 = no touch, Z>0 = touching
/** How many "inputs" should we drop after intial touch and liftoff? */
#define TOUCH_INPUT_BUFFER 10
/** How many times we have registered a touch input. */
static uint16_t touch_count = 0;
/** Touch inputs waiting to be drawn (insane asylum) */
static uint16_t touch_queue_x[TOUCH_INPUT_BUFFER] = {0}, touch_queue_y[TOUCH_INPUT_BUFFER] = {0};
/** Last written value(s) in the touch queue. */
static uint8_t touch_queue_lastwrite_position = 0;
/** These buttons select the current active color. */
LGFX_Button colorButton[16];
LGFX_Button menuButton[5];
LGFX_Button toolButton[5];
static char *toolButtonLabel[5] = {"Pencil", "Brush", "Dither", "Fill", "Noise"};
LGFX_Button actionButton[5];
static char *actionButtonLabel[5] = {"Tools", "Clear", "Send", "Save", "Load"};
LGFX_Button saveButton[5];
static char *saveButtonLabel[5] = {"Slot 1", "Slot 2", "Slot 3", "Slot 4", "Slot 5"};
LGFX_Button loadButton[5];
static char *loadButtonLabel[5] = {"Slot 1", "Slot 2", "Slot 3", "Slot 4", "Slot 5"};
// save slots!!!!

// Input (Buttons)
/** Which GPIO pin will be used as input for the menu button? */
#define MENU_BUTTON_PIN 0
/** How long should button be pressed before logically registering input? */
#define DEBOUNCE_MILLISECONDS 50
/** Variable for incoming serial command. */
String serialCommand;

// Output (LED)

// Display
/** Reference to display object. */
static LGFX tft;
#define TFT_HOR_RES 480
#define TFT_VER_RES 320
uint8_t *canvas_framebuffer;
/** Defines tools that we can use on the canvas. */
typedef enum
{
  TOOL_ERASER,
  TOOL_BRUSH,
  TOOL_PENCIL,
  TOOL_DITHER,
  TOOL_FILL
} draw_tool_id_t;
/** Defines the menus we can be in. */
typedef enum
{
  SCREEN_CANVAS,
  SCREEN_CANVAS_MENU,
  SCREEN_WELCOME,
  SCREEN_NETWORK_SETTINGS
} screen_id_t;
/** Defines what dropdown (if any) is visible in SCREEN_CANVAS_MENU */
typedef enum
{
  DROPDOWN_NONE,
  DROPDOWN_TOOLS,
  DROPDOWN_CLEAR,
  DROPDOWN_SEND,
  DROPDOWN_SAVE,
  DROPDOWN_LOAD
} canvas_dropdown_id_t;
static screen_id_t active_screen;
static canvas_dropdown_id_t active_dropdown;

/** Defines color palette for our 4-bit color frame buffer.
 * Color palette is from https://androidarts.com/palette/16pal.htm
 */
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

// Canvas
static int currentBackgroundColorIndex = 0;
static int currentDrawColorIndex = 2;
static int currentBrushRadius = 5;

// UI
#define CANVAS_DRAW_MENU_TOP_BAR_DIST_FROM_TOP_PX 5
#define CANVAS_DRAW_MENU_TOP_BAR_HEIGHT_PX 50
#define CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX 80
#define CANVAS_DRAW_MENU_BOTTOM_BAR_DIST_FROM_BOTTOM_PX 0
#define CANVAS_DRAW_MENU_BOTTOM_BAR_HEIGHT_PX 25
#define CANVAS_DRAW_MENU_BOTTOM_BAR_ITEM_WIDTH_PX 27
#define CANVAS_DRAW_MENU_DROPDOWN_DIST_BETWEEN_ITEMS 5

// Network
#define LOCAL_HOSTNAME "friendbox"

// Storage
SPIClass sdspi = SPIClass(HSPI);
#define SD_CS 26
#define SD_SCK 14
#define SD_MISO 32
#define SD_MOSI 13

// Functions
void updateDisplayWithFB();
void handleMenuButton(bool recheckInput);
void setDrawColor(uint8_t colorIndex);
void drawPixelToFB(int x, int y, uint8_t colorIndex);
void drawBrushToFB(int x, int y, int radius, uint8_t colorIndex);
void drawCanvasMenu(bool show);
void drawCanvasMenuTools();
void drawClearScreen();
void drawCanvasMenuSave();
void drawCanvasMenuLoad();
void saveImageToSD(int slot);
void loadImageFromSD(int slot);

/** Read from the display, and queue touch points if valid. */
void handleTouch()
{
  uint16_t localTouchX, localTouchY;
  // delay(2); // Introducing delay for some reason eliminates crap lines on the screen?? This is an SPI bus iss
  if (tft.getTouch(&localTouchX, &localTouchY) && (localTouchX >= 0 && localTouchX < TFT_HOR_RES &&
                                                   localTouchY >= 0 && localTouchY < TFT_VER_RES))
  { // Touching in bounds
    // Serial.print("Touch - X: ");
    // Serial.print(localTouchX);
    // Serial.print(" Y: ");
    // Serial.println(localTouchY);

    // Drop inputs until we exceed TOUCH_INPUT_BUFFER, this prevents smearing from pen pressing on screen.
    if (++touch_count > TOUCH_INPUT_BUFFER)
    {
      if (touch_queue_lastwrite_position < TOUCH_INPUT_BUFFER)
      {
        touch_queue_x[touch_queue_lastwrite_position] = localTouchX;
        touch_queue_y[touch_queue_lastwrite_position] = localTouchY;

        touch_queue_lastwrite_position =
            (touch_queue_lastwrite_position + 1) % TOUCH_INPUT_BUFFER;
      }
      // Draw points in queue if valid.
      uint8_t touch_queue_read_position = (touch_queue_lastwrite_position + TOUCH_INPUT_BUFFER - (TOUCH_INPUT_BUFFER - 1)) % TOUCH_INPUT_BUFFER;

      // Serial.print("Last Write Pos: ");
      // Serial.println(touch_queue_lastwrite_position);
      // Serial.print("Read Pos: ");
      // Serial.println(touch_queue_read_position);
      // Serial.print("X Value: ");
      // Serial.println(touch_queue_x[touch_queue_read_position]);

      if (((touch_queue_x[touch_queue_read_position] +
            touch_queue_y[touch_queue_read_position]) > 0))
      {
        touchX = touch_queue_x[touch_queue_read_position];
        touchY = touch_queue_y[touch_queue_read_position];
        touchZ = 1;
        if (active_screen == SCREEN_CANVAS)
        {
          drawBrushToFB(touchX, touchY, 3, currentDrawColorIndex);
        }
      }
    }
  }
  else
  { // No longer touching, re-init to zero. Also drops last 10 inputs, preventing smearing.
    touch_count = 0;
    memset(touch_queue_x, 0, sizeof(touch_queue_x));
    memset(touch_queue_y, 0, sizeof(touch_queue_y));
    touchX = 0;
    touchY = 0;
    touchZ = 0;
  }
}

/** Handle button logic based on what screen we are on. */
void handleTouchButtonUpdate()
{
  if (active_screen == SCREEN_CANVAS_MENU)
  {
    for (uint8_t b = 0; b < 16; b++)
    {
      bool touching = touchZ && colorButton[b].contains(touchX, touchY);
      colorButton[b].press(touching);
      if (colorButton[b].justReleased())
      {
        colorButton[b].drawButton(false);
        Serial.println("Unpress.");
      }

      if (colorButton[b].justPressed())
      {
        colorButton[b].drawButton(true);
        Serial.println("Press.");
        setDrawColor(b);
        drawCanvasMenuTools();
      }
    }
    for (uint8_t b = 0; b < 5; b++)
    {
      bool touching = touchZ && actionButton[b].contains(touchX, touchY);
      actionButton[b].press(touching);

      if (actionButton[b].justReleased())
      {
        actionButton[b].drawButton();
      }

      if (actionButton[b].justPressed())
      {
        actionButton[b].drawButton(true);
        switch (b)
        {
        case 2:
          drawClearScreen();
          drawCanvasMenu(true);
          break;
        case 3:
          // saveImageToSD(0);
          drawCanvasMenuSave();
          break;
        case 4:
          // loadImageFromSD(0);
          drawCanvasMenuLoad();
          // drawCanvasMenu(true);
          // Serial.println("Loading ");
          break;
        default:
          break;
        }
        // setDrawColor(b);
      }
    }
    if (active_dropdown == DROPDOWN_TOOLS)
    {
      for (uint8_t b = 0; b < 5; b++)
      {

        if (toolButton[b].justReleased())
        {
          toolButton[b].drawButton(); // draw normal
        }

        if (toolButton[b].justPressed())
        {
          toolButton[b].drawButton(true); // draw invert
          Serial.println("Tool Button Pressed.");
        }
      }
    }
    if (active_dropdown == DROPDOWN_LOAD) {
      for (uint8_t b = 0; b < 5; b++)
      {

        if (toolButton[b].justReleased())
        {
          toolButton[b].drawButton(); // draw normal
          drawCanvasMenu(true);
        }

        if (toolButton[b].justPressed())
        {
          toolButton[b].drawButton(true); // draw invert
          Serial.println("Load Button Pressed.");
        }
      }
    }
  }
}

/** Handle pressing of hardware button, will implement as hall effect sensor later 
 * recheckInput will register another logical press even if button is being held.
*/
void handleMenuButton(bool recheckInput)
{
  /** Debouncing nonsense. */
  static unsigned long lastPress = 0;
  /** Debouncing nonsense. */
  static unsigned int lastButtonState = 0;
  /** Debouncing nonsense. */
  static bool alreadyPressed = false;
  if (digitalRead(MENU_BUTTON_PIN) == LOW) /*Button Pressed*/
  {
    if (lastPress == 0)
    {
      lastPress = millis();
    }
    if (((millis() >= (lastPress + DEBOUNCE_MILLISECONDS)) & !alreadyPressed) || recheckInput)
    {
      Serial.println("Logical Press");
      drawCanvasMenu(true);
      alreadyPressed = true;
    }
    else
    {
      return;
    }
  }
  else /*Button Released*/
  {
    if (lastPress)
    {
      lastPress = 0;
      alreadyPressed = false;
      Serial.println("Logical Release.");
      drawCanvasMenu(false);
    }
  }
}

void drawCanvasMenu(bool show)
{
  if (show)
  {
    active_screen = SCREEN_CANVAS_MENU;
    // tft.fillRect(0, 0, 480, 50, TFT_WHITE);
    // tft.fillRect(0, 270, 480, 50, TFT_WHITE);
    for (int col = 0; col < 16; col++)
    {
      colorButton[col].initButtonUL(&tft, (col * 30), (tft.height() - CANVAS_DRAW_MENU_BOTTOM_BAR_HEIGHT_PX),
                                    CANVAS_DRAW_MENU_BOTTOM_BAR_ITEM_WIDTH_PX, CANVAS_DRAW_MENU_BOTTOM_BAR_HEIGHT_PX,
                                    TFT_WHITE, (int)draw_color_palette[col], TFT_WHITE, "", 1, 1);
      colorButton[col].drawButton();
    }
    drawCanvasMenuTools();
  }
  else
  {
    active_screen = SCREEN_CANVAS;
    updateDisplayWithFB();
  }
}

void drawCanvasMenuTools()
{
  active_dropdown == DROPDOWN_NONE;
  for (int col = 0; col < 5; col++)
  {
    actionButton[col].initButtonUL(&tft, (col * 96), 10,
                                   CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX, CANVAS_DRAW_MENU_TOP_BAR_HEIGHT_PX,
                                   TFT_WHITE, (int)draw_color_palette[currentDrawColorIndex], TFT_WHITE, actionButtonLabel[col], 2, 2);
    actionButton[col].drawButton();
  }
}

void drawCanvasMenuSave()
{
  active_dropdown == DROPDOWN_SAVE;
  for (int col = 0; col < 5; col++)
  {
    saveButton[col].initButtonUL(&tft, (3 * 96), (col * 40 + CANVAS_DRAW_MENU_TOP_BAR_HEIGHT_PX + CANVAS_DRAW_MENU_TOP_BAR_DIST_FROM_TOP_PX + CANVAS_DRAW_MENU_DROPDOWN_DIST_BETWEEN_ITEMS),
                                 CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX, CANVAS_DRAW_MENU_TOP_BAR_HEIGHT_PX,
                                 TFT_WHITE, (int)draw_color_palette[currentDrawColorIndex], TFT_WHITE, saveButtonLabel[col], 2, 2);
    saveButton[col].drawButton();
  }
}

void drawCanvasMenuLoad()
{
  active_dropdown == DROPDOWN_LOAD;
  for (int col = 0; col < 5; col++)
  {
    loadButton[col].initButtonUL(&tft, (4 * 96), (col * 40 + CANVAS_DRAW_MENU_TOP_BAR_HEIGHT_PX + CANVAS_DRAW_MENU_TOP_BAR_DIST_FROM_TOP_PX + CANVAS_DRAW_MENU_DROPDOWN_DIST_BETWEEN_ITEMS),
                                 CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX, CANVAS_DRAW_MENU_TOP_BAR_HEIGHT_PX,
                                 TFT_WHITE, (int)draw_color_palette[currentDrawColorIndex], TFT_WHITE, loadButtonLabel[col], 2, 2);
    loadButton[col].drawButton();
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

void initDisplay()
{
#ifdef FRIENDBOX_DEBUG_MODE
  Serial.println("INFO: Initializing LGFX...");
#endif
  tft.init();
  tft.setRotation(3); // This option enables suffering. Don't forget to account for coordinate translation!
  tft.setBrightness(255);
  tft.setColorDepth(16);
  tft.fillScreen(TFT_DARKCYAN);
  tft.setTextColor(TFT_GOLD, TFT_DARKCYAN);
  tft.setTextSize(4);
  tft.drawCenterString("FriendBox", 240, 120);
  tft.setTextSize(3);
  tft.drawCenterString(FRIENDBOX_SOFTWARE_VERSION, 240, 160);

  // Allocate framebuffer in ROTATED dimensions
  canvas_framebuffer = (uint8_t *)malloc((tft.width() * tft.height()) / 2); // 76.8 KB
  if (!canvas_framebuffer)
  {
    Serial.println("FATAL: Framebuffer allocation failed!");
    while (1)
      ;
  }
  memset(canvas_framebuffer, 0, (tft.width() * tft.height()) / 2);
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
    // SD.end();
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
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(160, 40);
    tft.setTextSize(4);
    tft.drawCenterString("Touch Calibration", 240, 70);
    tft.setTextSize(2);
    tft.drawCenterString("Press Highlighted Corners...", 240, 120);

    tft.calibrateTouch(calibration_data, TFT_WHITE, TFT_RED, 15);

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

  tft.setTouchCalibrate(calibration_data);
}

void drawPixelToFB(int x, int y, uint8_t colorIndex)
{
  if (x < 0 || x >= tft.width() || y < 0 || y >= tft.height())
    return;

  int index = y * tft.width() + x;
  int byteIndex = index >> 1;

  if (index & 1)
    canvas_framebuffer[byteIndex] = (canvas_framebuffer[byteIndex] & 0xF0) | (colorIndex & 0x0F);
  else
    canvas_framebuffer[byteIndex] = (canvas_framebuffer[byteIndex] & 0x0F) | ((colorIndex & 0x0F) << 4);
}

// Helper functions to change tool settings
void setDrawColor(uint8_t colorIndex)
{
  if (colorIndex < 16)
  {
    Serial.print("Color set to: ");
    Serial.println(colorIndex);
    currentDrawColorIndex = colorIndex;
  }
}

void setBackgroundColor(uint8_t colorIndex)
{
  if (colorIndex < 16)
  {
    Serial.print("Background color set to: ");
    Serial.println(colorIndex);
    currentBackgroundColorIndex = colorIndex;
    tft.fillScreen(draw_color_palette[colorIndex]);
  }
}

void setBrushRadius(int radius)
{
  if (radius > 0 && radius <= 50)
  {
    Serial.print("Brush radius set to: ");
    Serial.println(radius);
  }
}

/** Draw a circle brush at x,y with given radius and color - Updates BOTH framebuffer and screen in real-time! */
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
        drawPixelToFB(px, py, colorIndex);

        // Draw to screen immediately for instant feedback
        if (px >= 0 && px < tft.width() && py >= 0 && py < tft.height())
        {
          tft.drawPixel(px, py, draw_color_palette[colorIndex]);
        }
      }
    }
  }
}

void drawToolToFB(int radius, draw_tool_id_t tool, uint8_t colorIndex)
{

  switch (tool)
  {
  case TOOL_BRUSH:
    break;
  default:
    Serial.println("WARN: Trying to draw with an invalid / not implemented tool.");
    break;
  }
}

void drawTest4()
{
  for (int y = 0; y < tft.height(); y++)
  {
    for (int x = 0; x < tft.width(); x++)
    {
      uint8_t color = (x >> 5) & 0x0F; // 16 vertical stripes
      drawPixelToFB(x, y, color);
    }
  }
}

void drawClearScreen()
{
  for (int y = 0; y < tft.height(); y++)
  {
    for (int x = 0; x < tft.width(); x++)
    {
      drawPixelToFB(x, y, 0);
    }
  }
  updateDisplayWithFB();
}

void saveImageToSD(int slot)
{
  char filename[20];
  snprintf(filename, sizeof(filename), "/slot%d.bin", slot);

  File f = SD.open(filename, FILE_WRITE);
  if (f)
  {
    f.write(canvas_framebuffer, (tft.width() * tft.height()) / 2);
    f.close();
    #ifdef FRIENDBOX_DEBUG_MODE
    Serial.print("Saved image to save slot ");
    Serial.print(slot);
    Serial.println("!");
    #endif
  }
}

void loadImageFromSD(int slot)
{
  char filename[20];
  snprintf(filename, sizeof(filename), "/slot%d.bin", slot);

  File f = SD.open(filename, FILE_READ);
  if (f)
  {
    f.read(canvas_framebuffer, (tft.width() * tft.height()) / 2);
    f.close();
    updateDisplayWithFB();
    #ifdef FRIENDBOX_DEBUG_MODE
    Serial.print("Loaded image from save slot ");
    Serial.print(slot);
    Serial.println("!");
    #endif
  }
}

/** Wipe entire screen and replace with contents of the 4-bit color framebuffer. */
void updateDisplayWithFB()
{
  tft.startWrite();
  tft.setAddrWindow(0, 0, tft.width(), tft.height());

  for (int i = 0; i < (tft.width() * tft.height()) / 2; i++)
  {
    uint8_t b = canvas_framebuffer[i];
    tft.pushColor(draw_color_palette[b >> 4]);
    tft.pushColor(draw_color_palette[b & 0x0F]);
  }

  tft.endWrite();
}

void handleSerialCommand()
{
  if (Serial.available())
  {
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
      saveImageToSD(0);
    }
    else if (serialCommand.equals("load"))
    {
      loadImageFromSD(0);
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
  initSD(false);
  initTouch(false);
  // initNetwork(NETWORK_SSID, NETWORK_PASS, LOCAL_HOSTNAME);
  pinMode(MENU_BUTTON_PIN, INPUT);
}

void loop()
{
  handleTouch();
  handleTouchButtonUpdate();
  handleSerialCommand();
  // We just gotta run this on loop until we can set up interrupts.
  handleMenuButton(false);
}
