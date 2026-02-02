#include "FS.h"
#include <Arduino.h>
#include <WiFi.h>
#include <LovyanGFX.h>
#include <LGFX_ESP32_ST7796S_XPT2046.hpp>
#include <SPI.h>
#include <SD.h>
#include <ESP32I2SAudio.h>
#include <BackgroundAudio.h>
#include "secrets.h"

// (C) 2025-2026 Brandon Bunce - FriendBox System Software
#define FRIENDBOX_DEBUG_MODE true
#define FRIENDBOX_SOFTWARE_VERSION "Software v0.2 (NO-LVGL)"

// Touch
uint16_t touchX, touchY, touchZ; // Z:0 = no touch, Z>0 = touching
// Stores millis() value from last recorded input.
static unsigned long lastTouch = 0;
// For how many milliseconds after last input should we count before registering release?
#define UI_TOUCH_INPUT_BUFFER_MS 
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
#define ACTION_BUTTON_COUNT 4
LGFX_Button actionButton[ACTION_BUTTON_COUNT];
static const char *actionButtonLabel[ACTION_BUTTON_COUNT] = {"Tools", "Send", "Save", "Load"};
#define TOOL_DROPDOWN_BUTTON_COUNT 6
LGFX_Button toolButton[TOOL_DROPDOWN_BUTTON_COUNT];
static const char *toolButtonLabel[TOOL_DROPDOWN_BUTTON_COUNT] = {"Pencil", "Brush", "Fill", "Noise", "Dither", "Sticker"};
LGFX_Button settingsButton;
static const char *settingsButtonLabel = "Set Size";
#define SAVE_DROPDOWN_BUTTON_COUNT 7
LGFX_Button saveButton[SAVE_DROPDOWN_BUTTON_COUNT];
static const char *saveButtonLabel[SAVE_DROPDOWN_BUTTON_COUNT] = {"Slot 1", "Slot 2", "Slot 3", "Slot 4", "Slot 5", "Slot 6", "Slot 7"};
#define LOAD_DROPDOWN_BUTTON_COUNT 7
LGFX_Button loadButton[LOAD_DROPDOWN_BUTTON_COUNT];
static const char *loadButtonLabel[LOAD_DROPDOWN_BUTTON_COUNT] = {"Slot 1", "Slot 2", "Slot 3", "Slot 4", "Slot 5", "Slot 6", "Slot 7"};
static int lastActiveSlot = 0;
// Input (Buttons)** Which GPIO pin will be used as input for the menu button? */
#define MENU_BUTTON_PIN 0
#define HALL_SENSOR_PIN 27
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
  TOOL_PENCIL,
  TOOL_BRUSH,
  TOOL_FILL,
  TOOL_NOISE,
  TOOL_DITHER,
  TOOL_STICKER
} draw_tool_id_t;
/** Defines the menus we can be in. */
typedef enum
{
  SCREEN_CANVAS,
  SCREEN_CANVAS_MENU,
  SCREEN_CANVAS_SIZE_SELECT,
  SCREEN_WELCOME,
  SCREEN_NETWORK_SETTINGS
} screen_id_t;
/** Defines what dropdown (if any) is visible in SCREEN_CANVAS_MENU */
typedef enum
{
  DROPDOWN_NONE,
  DROPDOWN_TOOLS,
  DROPDOWN_SEND,
  DROPDOWN_SAVE,
  DROPDOWN_LOAD
} canvas_dropdown_id_t;
static draw_tool_id_t active_tool = TOOL_BRUSH;
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

/** Defines text of color matching to palette for readability. */
uint16_t draw_color_palette_text_color[16] = {
    0xFFFF, // Black (0)
    0xFFFF, // Gray (1)
    0x0000, // White (2)
    0xFFFF, // Red (3)
    0xFFFF, // Meat (4)
    0xFFFF, // Dark Brown (5)
    0xFFFF, // Brown (6)
    0xFFFF, // Orange (7)
    0x0000, // Yellow (8)
    0xFFFF, // Dark Green (9)
    0xFFFF, // Green (10)
    0xFFFF, // Slime Green (11)
    0xFFFF, // Night Blue (12)
    0xFFFF, // Sea Blue (13)
    0xFFFF, // Sky Blue (14)
    0x0000  // Cloud Blue (15)
};

// Canvas
static int currentBackgroundColorIndex = 0;
static int currentDrawColorIndex = 2;
static int currentBrushRadius = 5;

// UI
#define CANVAS_DRAW_MENU_TOP_BAR_DIST_FROM_TOP_PX 5
#define CANVAS_DRAW_MENU_TOP_BAR_HEIGHT_PX 50
#define CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX 110
#define CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX 25
#define CANVAS_DRAW_MENU_BOTTOM_BAR_DIST_FROM_BOTTOM_PX 5
#define CANVAS_DRAW_MENU_BOTTOM_BAR_HEIGHT_PX 25
#define CANVAS_DRAW_MENU_BOTTOM_BAR_ITEM_WIDTH_PX 27
#define CANVAS_DRAW_MENU_DROPDOWN_DIST_BETWEEN_ITEMS 5

#define ACTION_BUTTON_SPACING ((TFT_HOR_RES - (ACTION_BUTTON_COUNT * CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX)) / (ACTION_BUTTON_COUNT + 1))
#define ACTION_BUTTON_X_POS(col) (ACTION_BUTTON_SPACING + ((col) * (CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX + ACTION_BUTTON_SPACING)))
#define COLOR_BUTTON_SPACING ((TFT_HOR_RES - (16 * CANVAS_DRAW_MENU_BOTTOM_BAR_ITEM_WIDTH_PX)) / 16)
#define COLOR_BUTTON_X_POS(col) (COLOR_BUTTON_SPACING + ((col) * (CANVAS_DRAW_MENU_BOTTOM_BAR_ITEM_WIDTH_PX + COLOR_BUTTON_SPACING)))

// Network
#define LOCAL_HOSTNAME "friendbox"

// Storage
SPIClass sdspi = SPIClass(HSPI);
#define SD_CS 26
#define SD_SCK 14
#define SD_MISO 32
#define SD_MOSI 13

// Audio
//const int audio_buff_size = 128;
//int available_bytes, read_bytes;
//uint8_t buffer[audio_buff_size];
//I2SClass I2S;
ESP32I2SAudio audio(16, 17, 5);
BackgroundAudioMP3 mp3(audio);
  static uint8_t soundbuffer[8192];

// Functions
void updateDisplayWithFB();
void handleMenuButton(bool recheckInput);
void setDrawColor(uint8_t colorIndex);
void drawPixelToFB(int x, int y, uint8_t colorIndex);
void drawBrushToFB(int x, int y, int radius, uint8_t colorIndex);
void drawCanvasMenu(bool show, bool wipeScreen, bool skipColorDraw, bool skipActionDraw, bool skipDropdownDraw);
void drawClearScreen();
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
    touchZ = millis();
    touchZ = 0;
  }
}

/** Draw to screen if within canvas context! */
void handleCanvasDraw()
{
  if (active_screen == SCREEN_CANVAS && touchZ)
  {
    drawBrushToFB(touchX, touchY, 3, currentDrawColorIndex);
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
        drawCanvasMenu(true, false, true, false, false);
      }
    }
    for (uint8_t b = 0; b < ACTION_BUTTON_COUNT; b++)
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
        case 0: // Tools
          // Implement cow tools, would be lacking in sophistication
          if (active_dropdown != DROPDOWN_TOOLS)
          {
            active_dropdown = DROPDOWN_TOOLS;
            drawCanvasMenu(true, true, false, false, false);
          }
          break;
        case 1: // Send
          // Implement send screen
          break;
        case 2: // Save
          if (active_dropdown != DROPDOWN_SAVE)
          {
            active_dropdown = DROPDOWN_SAVE;
            drawCanvasMenu(true, true, false, false, false);
          }
          break;
          break;
        case 3: // Load
          if (active_dropdown != DROPDOWN_LOAD)
          {
            active_dropdown = DROPDOWN_LOAD;
            drawCanvasMenu(true, true, false, false, false);
          }
          break;
          break;
        default:
          break;
        }
        // setDrawColor(b);
      }
    }

    switch (active_dropdown)
    {
    case DROPDOWN_TOOLS:
      for (uint8_t b = 0; b < TOOL_DROPDOWN_BUTTON_COUNT; b++)
      {
        bool touching = touchZ && toolButton[b].contains(touchX, touchY);
        toolButton[b].press(touching);

        if (toolButton[b].justReleased())
        {
          Serial.println("Tool Button Released.");
          if (!touchZ)
          {
            Serial.println("Tool Button Logical Release.");
            active_tool = draw_tool_id_t(b);
            drawCanvasMenu(true, true, false, false, false);
          }
          if (b != active_tool)
          {
            toolButton[b].drawButton(); // draw normal
          }
        }

        if (toolButton[b].justPressed())
        {
          toolButton[b].drawButton(true); // draw invert
          Serial.println("Tool Button Pressed.");
        }
      }
      break;
    case DROPDOWN_LOAD:
      for (uint8_t b = 0; b < LOAD_DROPDOWN_BUTTON_COUNT; b++)
      {
        bool touching = touchZ && loadButton[b].contains(touchX, touchY);
        loadButton[b].press(touching);

        if (loadButton[b].justReleased())
        {
          loadButton[b].drawButton(); // draw normal
          // drawCanvasMenu(true, false, true, false, false);
          if (!touchZ)
            loadImageFromSD(b);
        }

        if (loadButton[b].justPressed())
        {
          loadButton[b].drawButton(true); // draw invert
          Serial.println("Load Button Pressed.");
        }
      }
      if (!touchZ)
      {
        active_dropdown = DROPDOWN_NONE;
        drawCanvasMenu(true, true, false, false, false);
      }
      break;
    case DROPDOWN_SAVE:
      for (uint8_t b = 0; b < SAVE_DROPDOWN_BUTTON_COUNT; b++)
      {
        bool touching = touchZ && saveButton[b].contains(touchX, touchY);
        saveButton[b].press(touching);

        if (saveButton[b].justReleased())
        {
          saveButton[b].drawButton(); // draw normal
          // drawCanvasMenu(true, false, true, false, false);
          if (!touchZ)
            saveImageToSD(b);
        }

        if (saveButton[b].justPressed())
        {
          saveButton[b].drawButton(true); // draw invert
          Serial.println("Load Button Pressed.");
        }
      }
      if (!touchZ)
      {
        active_dropdown = DROPDOWN_NONE;
        drawCanvasMenu(true, true, false, false, false);
      }
      break;
    case DROPDOWN_NONE:
      // Do nothing!
      break;
    default:
#ifdef FRIENDBOX_DEBUG_MODE
      Serial.println("CRITICAL: active_dropdown is in an invalid state. Undefined behaviour is occurring!");
#endif
      break;
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
  if (digitalRead(HALL_SENSOR_PIN) == LOW) /*Button Pressed*/
  {
    if (lastPress == 0)
    {
      lastPress = millis();
    }
    if (((millis() >= (lastPress + DEBOUNCE_MILLISECONDS)) & !alreadyPressed) || recheckInput)
    {
      Serial.println("Logical Press");
      drawCanvasMenu(true, false, false, false, false);
      alreadyPressed = true;
    }
    else
    {
      return;
    }
  }
  else /*Button Released*/
  {
    if (lastPress && alreadyPressed)
    {
      lastPress = 0;
      alreadyPressed = false;
      Serial.println("Logical Release.");
      drawCanvasMenu(false, false, false, false, false);
    }
  }
}

/** Draws the canvas menu and appropriate sub-menus. Reduce excess drawing by passing parameters to skip things that are already drawn or haven't changed. */
void drawCanvasMenu(bool show, bool wipeScreen, bool skipColorDraw, bool skipActionDraw, bool skipDropdownDraw)
{
  if (show)
  {
    if (wipeScreen)
    {
      updateDisplayWithFB();
    }
    active_screen = SCREEN_CANVAS_MENU;

    if (!skipActionDraw)
    {
      // Draw action menu
      for (int col = 0; col < ACTION_BUTTON_COUNT; col++)
      {
        actionButton[col].initButtonUL(&tft, ACTION_BUTTON_X_POS(col), CANVAS_DRAW_MENU_TOP_BAR_DIST_FROM_TOP_PX,
                                       CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX, CANVAS_DRAW_MENU_TOP_BAR_HEIGHT_PX,
                                       TFT_WHITE, (int)draw_color_palette[currentDrawColorIndex], (int)draw_color_palette_text_color[currentDrawColorIndex], actionButtonLabel[col], 3, 3);
        actionButton[col].drawButton();
      }
    }

    if (!skipColorDraw)
    {
      // Draw colors menu
      for (int col = 0; col < 16; col++)
      {
        colorButton[col].initButtonUL(&tft, COLOR_BUTTON_X_POS(col), (tft.height() - (CANVAS_DRAW_MENU_BOTTOM_BAR_HEIGHT_PX + CANVAS_DRAW_MENU_BOTTOM_BAR_DIST_FROM_BOTTOM_PX)),
                                      CANVAS_DRAW_MENU_BOTTOM_BAR_ITEM_WIDTH_PX, CANVAS_DRAW_MENU_BOTTOM_BAR_HEIGHT_PX,
                                      TFT_WHITE, (int)draw_color_palette[col], (int)draw_color_palette_text_color[currentDrawColorIndex], "", 1, 1);
        colorButton[col].drawButton();
      }
    }

    if (!skipDropdownDraw)
    {
      // Check to see which dropdown is active and draw accordingly.
      switch (active_dropdown)
      {
      case DROPDOWN_TOOLS:
        for (int col = 0; col < TOOL_DROPDOWN_BUTTON_COUNT; col++)
        {
          toolButton[col].initButtonUL(&tft, ACTION_BUTTON_X_POS(0),
                                       ((col + 1) * (CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX + CANVAS_DRAW_MENU_DROPDOWN_DIST_BETWEEN_ITEMS) + CANVAS_DRAW_MENU_TOP_BAR_DIST_FROM_TOP_PX + CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX),
                                       CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX, CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX,
                                       TFT_WHITE, (int)draw_color_palette[currentDrawColorIndex], (int)draw_color_palette_text_color[currentDrawColorIndex], toolButtonLabel[col], 2, 2);
          if (col == active_tool)
          {
            toolButton[col].drawButton(true);
          }
          else
          {
            toolButton[col].drawButton();
          }
        }

        // Now, we'll check to see what the active tool is to draw it as selected and also any settings related to it.
        switch (active_tool)
        {
        case TOOL_PENCIL:
          settingsButton.initButtonUL(&tft, ACTION_BUTTON_X_POS(1),
                                      ((1) * (CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX + CANVAS_DRAW_MENU_DROPDOWN_DIST_BETWEEN_ITEMS) + CANVAS_DRAW_MENU_TOP_BAR_DIST_FROM_TOP_PX + CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX),
                                      CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX, CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX,
                                      TFT_WHITE, (int)draw_color_palette[currentDrawColorIndex], (int)draw_color_palette_text_color[currentDrawColorIndex], settingsButtonLabel, 2, 2);
          settingsButton.drawButton();
          break;
          break;
        case TOOL_BRUSH:
          settingsButton.initButtonUL(&tft, ACTION_BUTTON_X_POS(1),
                                      ((2) * (CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX + CANVAS_DRAW_MENU_DROPDOWN_DIST_BETWEEN_ITEMS) + CANVAS_DRAW_MENU_TOP_BAR_DIST_FROM_TOP_PX + CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX),
                                      CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX, CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX,
                                      TFT_WHITE, (int)draw_color_palette[currentDrawColorIndex], (int)draw_color_palette_text_color[currentDrawColorIndex], settingsButtonLabel, 2, 2);
          settingsButton.drawButton();
          break;
        case TOOL_FILL:
          break;
        case TOOL_NOISE:
          break;
        case TOOL_DITHER:
          break;
        case TOOL_STICKER:
          break;
        }
        break;
      case DROPDOWN_SEND:
        break;
      case DROPDOWN_SAVE:
        for (int col = 0; col < SAVE_DROPDOWN_BUTTON_COUNT; col++)
        {
          saveButton[col].initButtonUL(&tft, ACTION_BUTTON_X_POS(2),
                                       ((col + 1) * (CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX + CANVAS_DRAW_MENU_DROPDOWN_DIST_BETWEEN_ITEMS) + CANVAS_DRAW_MENU_TOP_BAR_DIST_FROM_TOP_PX + CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX),
                                       CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX, CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX,
                                       TFT_WHITE, (int)draw_color_palette[currentDrawColorIndex], (int)draw_color_palette_text_color[currentDrawColorIndex], saveButtonLabel[col], 2, 2);
          saveButton[col].drawButton();
        }
        break;
      case DROPDOWN_LOAD:
        for (int col = 0; col < LOAD_DROPDOWN_BUTTON_COUNT; col++)
        {
          loadButton[col].initButtonUL(&tft, ACTION_BUTTON_X_POS(3),
                                       ((col + 1) * (CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX + CANVAS_DRAW_MENU_DROPDOWN_DIST_BETWEEN_ITEMS) + CANVAS_DRAW_MENU_TOP_BAR_DIST_FROM_TOP_PX + CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX),
                                       CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX, CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX,
                                       TFT_WHITE, (int)draw_color_palette[currentDrawColorIndex], (int)draw_color_palette_text_color[currentDrawColorIndex], loadButtonLabel[col], 2, 2);
          loadButton[col].drawButton();
        }
        break;
      case DROPDOWN_NONE:
        // Do nothing!
        break;
      default:
#ifdef FRIENDBOX_DEBUG_MODE
        Serial.println("CRITICAL: active_dropdown is in an invalid state. Undefined behaviour is occurring!");
#endif
        break;
      }
    }
  }
  else
  {
    // Not showing menu, so reset contexts and flush display.
    active_screen = SCREEN_CANVAS;
    active_dropdown = DROPDOWN_NONE;
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
    tft.setTextSize(2);
    tft.setCursor(0, 5);
    tft.println("FATAL: SD mount failed.");
    tft.print("Re-attempting.");
    while (!SD.begin(SD_CS, sdspi)) {
      tft.print(".");
      delay(1000);
    }
    tft.println("SD was mounted!");
    delay(500);
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

void initSound() {
  Serial.println("playing sound");
  File startup_sound_file = SD.open("/pumpkin.wav", FILE_READ);
  startup_sound_file.read(soundbuffer, 8192);
  startup_sound_file.close();
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
    lastActiveSlot = slot;
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
    lastActiveSlot = slot;
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

void setup()
{
  // cawkins was here
  Serial.begin(115200);
  initDisplay();
  initSD(false);
  initTouch(false);
  //initSound();
  // initNetwork(NETWORK_SSID, NETWORK_PASS, LOCAL_HOSTNAME);
  // pinMode(MENU_BUTTON_PIN, INPUT);
  pinMode(HALL_SENSOR_PIN, INPUT_PULLUP);
  loadImageFromSD(lastActiveSlot);
}

void loop()
{
  handleTouch();
  handleCanvasDraw();
  handleTouchButtonUpdate();
  // We just gotta run this on loop until we can set up interrupts.
  handleMenuButton(false);
  //audio.write(soundbuffer, 8192);
}
