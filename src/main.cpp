#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LovyanGFX.h>
// #include <AceRoutine.h>
#include <LGFX_ESP32_ST7796S_XPT2046.hpp>
#include <SPI.h>
#include <SD.h>
#include "secrets.h"
#include <Preferences.h>
#include <vector>

// (C) 2025-2026 Brandon Bunce - FriendBox System Software
#define FRIENDBOX_DEBUG_MODE true
#define FRIENDBOX_SOFTWARE_VERSION "Software v0.3"

// Input (Buttons)
/** Which GPIO pin will be used as input for the hall effect button? */
#define HALL_SENSOR_PIN 27
/** How long should button be pressed before logically registering input? */
#define DEBOUNCE_MILLISECONDS 50

// Output (LED)
// To be implemented.

// Display
static LGFX tft;
#define TFT_HOR_RES 480
#define TFT_VER_RES 320

/* Stores our image drawing buffer. About ~76.8kb! */
uint8_t *canvas_framebuffer;

/** Defines tools that we can use on the canvas. */
typedef enum
{
  TOOL_PENCIL,
  TOOL_BRUSH,
  TOOL_FILL,
  TOOL_RAINBOW,
  TOOL_DITHER,
  TOOL_STICKER
} draw_tool_id_t;

/** Defines the UI context we are currently in */
typedef enum
{
  SCREEN_CANVAS,
  SCREEN_CANVAS_MENU,
  SCREEN_CANVAS_SIZE_SELECT,
  SCREEN_SEND,
  SCREEN_FILE_BROWSER,
  SCREEN_SYSTEM_MESSAGE,
  SCREEN_RECEIVED,
  SCREEN_WELCOME,
  SCREEN_STARTUP,
  SCREEN_NETWORK_SETTINGS
} screen_id_t;

/* Defines what dropdown we're in in SCREEN_CANVAS_MENU. */
typedef enum
{
  DROPDOWN_NONE,
  DROPDOWN_MENU,
  DROPDOWN_TOOLS,
  DROPDOWN_SAVE,
  DROPDOWN_LOAD
} dropdown_id_t;

/**
 * @param ACT_ON_PRESS Action is executed the moment the button is pressed.
 * @param ACT_ON_HOVER_AND_RELEASE Action is executed when hovering button and then stopping touch. Make sure you add additional logic to run this another time to register release.
 * @param ACT_ON_RELEASE Action is executed when button is released, regardless if screen is still being touched.
 */
typedef enum
{
  ACT_ON_PRESS,
  ACT_ON_HOVER_AND_RELEASE,
  ACT_ON_RELEASE
} ui_button_mode_id_t;

typedef enum
{
  SLIDE_FROM_TOP,
  SLIDE_FROM_BOTTOM,
  SLIDE_FROM_LEFT,
  SLIDE_FROM_RIGHT
} ui_anim_mode_id_t;

struct UIButton
{
  LGFX_Button button;
  int x, y, w, h;
  bool isDrawn = false;
  screen_id_t screenContext;
  dropdown_id_t dropdownContext = DROPDOWN_NONE;
  int fillColor;
};

struct Friend
{
  String name;
  int userID;
};

std::vector<UIButton *> uiButtons;

UIButton *lastPressedButton;

static draw_tool_id_t currentTool = TOOL_BRUSH;
static screen_id_t currentScreen = SCREEN_STARTUP;
static screen_id_t lastScreen; // Used by drawFriendboxLoadingScreen to return to previous context after showing loading screen.
static dropdown_id_t currentDropdown = DROPDOWN_NONE;

/** Defines color palette for our 4-bit color frame buffer.
 *
 * @param 0 Black
 * @param 1 Gray
 * @param 2 White
 * @param 3 Red
 * @param 4 Meat
 * @param 5 Dark Brown
 * @param 6 Brown
 * @param 7 Orange
 * @param 8 Yellow
 * @param 9 Dark Green
 * @param 10 Green
 * @param 11 Slime Green
 * @param 12 Night Blue
 * @param 13 Sea Blue
 * @param 14 Sky Blue
 * @param 15 Cloud Blue
 *
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

uint16_t draw_rainbow_palette_index[7] = {
    3,
    7,
    8,
    10,
    14,
    13,
    4};

/** Defines text of color corresponding to palette for readability. */
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
static int currentDrawColorIndex = 3;
static int currentRainbowPaletteIndex = 0;
static int currentBrushRadius = 5;
static int currentSaveSlot = 0;

// Touch
uint16_t touchX, touchY, touchZ; // Z:0 = no touch, Z>0 = touching
// Stores millis() value from last recorded input.
static unsigned long lastTouchTime = 0;
// For how many milliseconds after last input should we count before registering release?
#define UI_TOUCH_INPUT_BUFFER_MS
/** How many "inputs" should we drop after intial touch and liftoff? */
#define TOUCH_INPUT_BUFFER 10
/** Stores how many times we have registered a touch input. */
static uint16_t touch_count = 0;
/** Touch inputs waiting to be drawn (insane asylum) */
static uint16_t touch_queue_x[TOUCH_INPUT_BUFFER] = {0}, touch_queue_y[TOUCH_INPUT_BUFFER] = {0};
/** Last written value(s) in the touch queue. */
static uint8_t touch_queue_lastwrite_position = 0;

/* SCREEN_CANVAS_MENU */
#define SCREEN_CANVAS_UI_ACTION_BAR_DIST_FROM_TOP_PX 5
#define SCREEN_CANVAS_UI_ACTION_BAR_HEIGHT 50
#define SCREEN_CANVAS_UI_ACTION_BAR_ITEM_WIDTH_PX 110
#define SCREEN_CANVAS_UI_ACTION_BAR_ITEM_HEIGHT_PX 25
#define SCREEN_CANVAS_UI_ACTION_BUTTON_SPACING ((TFT_HOR_RES - (SCREEN_CANVAS_UI_ACTION_BUTTON_COUNT * SCREEN_CANVAS_UI_ACTION_BAR_ITEM_WIDTH_PX)) / (SCREEN_CANVAS_UI_ACTION_BUTTON_COUNT + 1))
#define SCREEN_CANVAS_UI_ACTION_BUTTON_X_POS(col) (SCREEN_CANVAS_UI_ACTION_BUTTON_SPACING + ((col) * (SCREEN_CANVAS_UI_ACTION_BAR_ITEM_WIDTH_PX + SCREEN_CANVAS_UI_ACTION_BUTTON_SPACING)))
#define SCREEN_CANVAS_UI_ACTION_BUTTON_COUNT 4
UIButton SCREEN_CANVAS_MENU_ACTION_BUTTON[SCREEN_CANVAS_UI_ACTION_BUTTON_COUNT];
static const char *SCREEN_CANVAS_MENU_ACTION_BUTTON_LABEL[SCREEN_CANVAS_UI_ACTION_BUTTON_COUNT] = {"Menu", "Tools", "Save", "Load"};

#define SCREEN_CANVAS_UI_COLOR_BAR_DIST_FROM_BOTTOM_PX 5
#define SCREEN_CANVAS_UI_COLOR_BAR_ITEM_HEIGHT_PX 25
#define SCREEN_CANVAS_UI_COLOR_BAR_ITEM_WIDTH_PX 27
#define SCREEN_CANVAS_UI_COLOR_BUTTON_SPACING ((TFT_HOR_RES - (16 * SCREEN_CANVAS_UI_COLOR_BAR_ITEM_WIDTH_PX)) / 16)
#define SCREEN_CANVAS_UI_COLOR_BUTTON_X_POS(col) (SCREEN_CANVAS_UI_COLOR_BUTTON_SPACING + ((col) * (SCREEN_CANVAS_UI_COLOR_BAR_ITEM_WIDTH_PX + SCREEN_CANVAS_UI_COLOR_BUTTON_SPACING)))
#define SCREEN_CANVAS_UI_COLOR_BUTTON_COUNT 16 // This should technically be static at 16 due to 4-bit color.
UIButton SCREEN_CANVAS_MENU_COLOR_BUTTON[SCREEN_CANVAS_UI_COLOR_BUTTON_COUNT];

#define CANVAS_DRAW_MENU_DROPDOWN_DIST_BETWEEN_ITEMS 5
#define TOOL_DROPDOWN_BUTTON_COUNT 6
UIButton SCREEN_CANVAS_MENU_TOOL_BUTTON[TOOL_DROPDOWN_BUTTON_COUNT];
static const char *SCREEN_CANVAS_MENU_TOOL_BUTTON_LABEL[TOOL_DROPDOWN_BUTTON_COUNT] = {"Pencil", "Brush", "Fill", "Rainbow", "Dither", "Pattern"};
#define SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON_COUNT 6
UIButton SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON_COUNT];
// static const char *SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON_LABEL = "Set Size";
#define MENU_DROPDOWN_BUTTON_COUNT 5
UIButton SCREEN_CANVAS_MENU_MENU_BUTTON[MENU_DROPDOWN_BUTTON_COUNT];
static const char *SCREEN_CANVAS_MENU_MENU_BUTTON_LABEL[MENU_DROPDOWN_BUTTON_COUNT] = {"Home", "Send", "Restart", "Files", "Network"};
#define SLOT_DROPDOWN_BUTTON_COUNT 7
UIButton SCREEN_CANVAS_MENU_SAVE_BUTTON[SLOT_DROPDOWN_BUTTON_COUNT];
static const char *SCREEN_CANVAS_MENU_SAVE_BUTTON_LABEL[SLOT_DROPDOWN_BUTTON_COUNT] = {"Slot 1", "Slot 2", "Slot 3", "Slot 4", "Slot 5", "Slot 6", "Slot 7"};
UIButton SCREEN_CANVAS_MENU_LOAD_BUTTON[SLOT_DROPDOWN_BUTTON_COUNT];
static const char *SCREEN_CANVAS_MENU_LOAD_BUTTON_LABEL[SLOT_DROPDOWN_BUTTON_COUNT] = {"Slot 1", "Slot 2", "Slot 3", "Slot 4", "Slot 5", "Slot 6", "Slot 7"};

/* SCREEN_SEND */
#define SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT 5
UIButton SCREEN_SEND_ADDRESSBOOK_BUTTON[SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT];
static const char *SCREEN_SEND_ADDRESSBOOK_BUTTON_LABEL[SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT] = {"Friend One", "Friend Two", "Friend Three", "Friend Four", "Friend Five"};
#define SCREEN_SEND_NAVI_BUTTON_COUNT 5
UIButton SCREEN_SEND_NAVI_BUTTON[SCREEN_SEND_NAVI_BUTTON_COUNT];
static const char *SCREEN_SEND_NAVI_BUTTON_LABEL[SCREEN_SEND_NAVI_BUTTON_COUNT] = {"Canvas", "Refresh", "Sort", "/\\", "\\/"};

/* SCREEN_FILE_BROWSER */
#define SCREEN_FILE_BROWSER_FILE_BUTTON_COUNT 5
UIButton SCREEN_FILE_BROWSER_FILE_BUTTON[SCREEN_FILE_BROWSER_FILE_BUTTON_COUNT];
static const char *SCREEN_FILE_BROWSER_FILE_BUTTON_LABEL[SCREEN_FILE_BROWSER_FILE_BUTTON_COUNT] = {"File One", "File Two", "File Three", "File Four", "File Five"};
#define SCREEN_FILE_BROWSER_NAVI_BUTTON_COUNT 4
UIButton SCREEN_FILE_BROWSER_NAVI_BUTTON[SCREEN_FILE_BROWSER_NAVI_BUTTON_COUNT];
static const char *SCREEN_FILE_BROWSER_NAVI_BUTTON_LABEL[SCREEN_FILE_BROWSER_NAVI_BUTTON_COUNT] = {"Back", "Sort", "/\\", "\\/"};

// Network
#define LOCAL_HOSTNAME "friendbox"
HTTPClient http;

struct UIList
{
  std::vector<std::string> listItems;
  int page;
};

UIList friendListUI;
UIList fileListUI;

// Storage
Preferences nvs; // https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/nvs_flash.html
SPIClass sdspi = SPIClass(HSPI);
#define SD_CS 26
#define SD_SCK 14
#define SD_MISO 32
#define SD_MOSI 13

// Audio
// Worry about this later.

// Functions
void handleMenuButton(bool recheckInput);
void setDrawColor(uint8_t colorIndex);
void drawPixelToFB(int x, int y, uint8_t colorIndex);
void drawBrushToFB(int x, int y, int radius, uint8_t colorIndex);
void drawDitherToFB(int x, int y, int radius, uint8_t colorIndex);
void initUIForScreen(screen_id_t targetScreen);
void drawScreenCanvasMenu();
void drawScreenSend(int page = 0);
void drawScreenFileBrowser(int page = 0);
void drawClearScreen();
bool drawSketchPreview(const char *filepath, int x, int y, int scaleDown, bool drawBorder = true);
void drawFriendboxLoadingScreen(const char *subtitle, int holdTimeMs = 0, const char *subsubtitle = "", const char *subsubsubtitle = "");
void saveImageToSD(int slot);
void loadSketchFromSD(const char *path);
void loadImageFromSD(int slot);
void drawFramebuffer(int x = 0, int y = 0, int w = TFT_HOR_RES, int h = TFT_VER_RES);
void networkSendFramebuffer(int userID);
void networkReceiveFramebuffer();
bool networkSendCanvas();
std::vector<std::string> sdGetFboxFiles();
std::vector<std::string> networkGetFriends();
void cleanupUIOutOfContext(bool destroyElement = false);
bool checkIfUIIsInitialized(screen_id_t targetScreen);
void changeScreenContext(screen_id_t targetScreen);
void drawTest4();
bool handleUIButtonPress(UIButton *targetButton, ui_button_mode_id_t buttonMode = ACT_ON_PRESS);
bool initSD(bool forceFormat);
bool initDisplay();
bool initTouch(bool forceCalibrate);
bool initNetwork(const char *netSSID, const char *netPassword, const char *hostname);
bool initNVS();

/** Read from the display, and queue touch points if valid. */
void handleTouch()
{
  uint16_t localTouchX, localTouchY;
  if (tft.getTouch(&localTouchX, &localTouchY) && (localTouchX >= 0 && localTouchX < TFT_HOR_RES &&
                                                   localTouchY >= 0 && localTouchY < TFT_VER_RES))
  { // Touching in bounds
    // Serial.print("Touch - X: ");
    // Serial.print(localTouchX);
    // Serial.print(" Y: ");
    // Serial.println(localTouchY);

    // Drop inputs until we exceed TOUCH_INPUT_BUFFER, this prevents smearing from pen applying pressure.
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
  { // No longer touching, re-init to zero. Also drops last 10 inputs to prevent smearing from pen removing pressure.
    touch_count = 0;
    memset(touch_queue_x, 0, sizeof(touch_queue_x));
    memset(touch_queue_y, 0, sizeof(touch_queue_y));
    lastTouchTime = millis();
    touchZ = 0;
  }
}

/** Draw to screen if within canvas context! */
void handleCanvasDraw()
{
  if (currentScreen == SCREEN_CANVAS && touchZ)
  {
    switch (currentTool)
    {
    case TOOL_PENCIL:
      drawBrushToFB(touchX, touchY, currentBrushRadius, currentDrawColorIndex);
      break;
    case TOOL_BRUSH:
      drawBrushToFB(touchX, touchY, currentBrushRadius, currentDrawColorIndex);
      break;
    case TOOL_FILL:
      drawClearScreen();
      break;
    case TOOL_RAINBOW: // Wouldn't be a bad idea to make this actually rainbow instead of cycling thru palette.... to follow ROYGBIV.
      drawBrushToFB(touchX, touchY, currentBrushRadius, draw_rainbow_palette_index[currentRainbowPaletteIndex]);
      currentRainbowPaletteIndex = (currentRainbowPaletteIndex + 1) % 7;
      break;
    case TOOL_DITHER:
      // Oh my god. why?
      drawDitherToFB(touchX, touchY, currentBrushRadius, currentDrawColorIndex);
      break;
    case TOOL_STICKER:
      drawTest4();
      drawFramebuffer();
      break;
    }
  }
}

/* Change brush size while keeping brush size above 0.*/
void changeBrushSize(int targetValue)
{
  if (targetValue > 0 && targetValue <= 100)
  {
    Serial.print("Adjusting brush size to ");
    Serial.println(targetValue);
    currentBrushRadius = targetValue;
  }
}

/** Run this to check the target button for inputs, and register a logical press when the button is pressed according to mode.
 * @param targetButton The button we are checking for input on.
 * @param buttonMode The mode we are checking for input in, this determines when we register a logical press.
 */
bool handleUIButtonPress(UIButton *targetButton, ui_button_mode_id_t buttonMode)
{
  // 1. Check if currently touching
  bool isTouching = touchZ && targetButton->button.contains(touchX, touchY);

  // 2. Update button state (this is what makes justPressed/justReleased work)
  targetButton->button.press(isTouching);

  // 3. Check for state transitions
  bool justReleased = targetButton->button.justReleased();
  bool justPressed = targetButton->button.justPressed();

  // 4. Draw visual feedback
  if (justReleased)
  {
    targetButton->button.drawButton(false);
  }
  if (justPressed)
  {
    targetButton->button.drawButton(true);
    lastPressedButton = targetButton;
  }

  // 5. Return based on mode
  switch (buttonMode)
  {
  case ACT_ON_PRESS:
    return justPressed; // Now correctly returns true only ONCE

  case ACT_ON_RELEASE:
    return justReleased;

  case ACT_ON_HOVER_AND_RELEASE:
    return justReleased && !touchZ;

  default:
    return false;
  }
}

void handleTouchUIUpdate()
{
  switch (currentScreen)
  {
  case SCREEN_CANVAS:
    // No buttons on canvas; do nothing.
    break;
  case SCREEN_CANVAS_MENU:
    // Handle logic for color bar
    for (uint8_t b = 0; b < 16; b++)
    {
      if (handleUIButtonPress(&SCREEN_CANVAS_MENU_COLOR_BUTTON[b], ACT_ON_PRESS))
      {
        setDrawColor(b);
        drawScreenCanvasMenu();
      }
    }
    // Handle logic for action bar.
    for (uint8_t b = 0; b < SCREEN_CANVAS_UI_ACTION_BUTTON_COUNT; b++)
    {
      if (handleUIButtonPress(&SCREEN_CANVAS_MENU_ACTION_BUTTON[b], ACT_ON_PRESS))
      {
        switch (b)
        {
        case 0: // Menu
          if (currentDropdown != DROPDOWN_MENU)
          {
            currentDropdown = DROPDOWN_MENU;
            cleanupUIOutOfContext();
          }
          break;
        case 1: // Tools, lacking in sophistication
          if (currentDropdown != DROPDOWN_TOOLS)
          {
            currentDropdown = DROPDOWN_TOOLS;
            cleanupUIOutOfContext();
          }
          break;
        case 2: // Save
          if (currentDropdown != DROPDOWN_SAVE)
          {
            currentDropdown = DROPDOWN_SAVE;
            cleanupUIOutOfContext();
          }
          break;
        case 3: // Load
          if (currentDropdown != DROPDOWN_LOAD)
          {
            currentDropdown = DROPDOWN_LOAD;
            cleanupUIOutOfContext();
          }
          break;
        default:
          break;
        }
        // Draw pressed dropdown.
        drawScreenCanvasMenu();
      }
    }
    // Handle dropdown buttons logic.
    if (!touchZ && currentDropdown != DROPDOWN_NONE)
    {
      if (lastPressedButton && lastPressedButton->dropdownContext == currentDropdown)
      {
        // Lets run thru logic again to check and see if we released.
        lastPressedButton = nullptr;
      }
      else
      {
        currentDropdown = DROPDOWN_NONE;
        cleanupUIOutOfContext();
        drawScreenCanvasMenu();
      }
    }
    switch (currentDropdown)
    {
    case DROPDOWN_NONE:
      break;
    case DROPDOWN_MENU:
      for (uint8_t b = 0; b < MENU_DROPDOWN_BUTTON_COUNT; b++)
      {
        if (handleUIButtonPress(&SCREEN_CANVAS_MENU_MENU_BUTTON[b], ACT_ON_HOVER_AND_RELEASE))
        {
          // saveImageToSD(b);
          switch (b)
          {
          case 0: // Home
            break;
          case 1: // Send
            changeScreenContext(SCREEN_SEND);
            return;
            break;
          case 2: // Reboot
            drawFriendboxLoadingScreen("Rebooting...", 500);
            esp_restart(); // obviously
            break;
          case 3: // Files
            changeScreenContext(SCREEN_FILE_BROWSER);
            return;
            break;
          }
        }
      }
      break;
    case DROPDOWN_TOOLS:
      for (uint8_t b = 0; b < TOOL_DROPDOWN_BUTTON_COUNT; b++)
      {
        if (handleUIButtonPress(&SCREEN_CANVAS_MENU_TOOL_BUTTON[b], ACT_ON_PRESS))
        {
          currentTool = draw_tool_id_t(b);
          SCREEN_CANVAS_MENU_ACTION_BUTTON[0].isDrawn = false;
          drawScreenCanvasMenu();
        }
      }
      for (uint8_t b = 0; b < SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON_COUNT; b++)
      {
        if (handleUIButtonPress(&SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[b], ACT_ON_PRESS))
        {
          switch (currentTool)
          {
          case TOOL_PENCIL:
          case TOOL_BRUSH:
          case TOOL_RAINBOW:
          case TOOL_DITHER:
            switch (b)
            {
            case 0:
              changeBrushSize(currentBrushRadius - 1);
              drawScreenCanvasMenu();
              break;
            case 1:
              changeBrushSize(currentBrushRadius + 1);
              drawScreenCanvasMenu();
              break;
            default:
              break;
            }
            break;
          case TOOL_FILL:
            break;
          case TOOL_STICKER:
            break;
          }
        }
      }
      break;
    case DROPDOWN_SAVE:
      for (uint8_t b = 0; b < SLOT_DROPDOWN_BUTTON_COUNT; b++)
      {
        if (handleUIButtonPress(&SCREEN_CANVAS_MENU_SAVE_BUTTON[b], ACT_ON_HOVER_AND_RELEASE))
        {
          changeScreenContext(SCREEN_CANVAS);
          saveImageToSD(b);
          changeScreenContext(SCREEN_CANVAS_MENU);
        }
      }
      break;
    case DROPDOWN_LOAD:
      for (uint8_t b = 0; b < SLOT_DROPDOWN_BUTTON_COUNT; b++)
      {
        /*if (handleUIButtonPress(&SCREEN_CANVAS_MENU_LOAD_BUTTON[b], ACT_ON_PRESS))
        {
          char filename[50];
          snprintf(filename, sizeof(filename), "/sketches/slots/slot%d.fbox", b);
          drawSketchPreview(filename, SCREEN_CANVAS_UI_ACTION_BUTTON_X_POS(1), 160, 4, true);
          lastPressedButton = nullptr;
        }*/
        if (handleUIButtonPress(&SCREEN_CANVAS_MENU_LOAD_BUTTON[b], ACT_ON_HOVER_AND_RELEASE))
        {
          changeScreenContext(SCREEN_CANVAS);
          loadImageFromSD(b);
          changeScreenContext(SCREEN_CANVAS_MENU);
        }
      }
      break;
    }
    break;
  case SCREEN_SEND:
    // Handle logic for send screen.
    for (uint8_t b = 0; b < SCREEN_SEND_NAVI_BUTTON_COUNT; b++)
    {
      switch (b)
      {
      case 0: // Canvas
        if (handleUIButtonPress(&SCREEN_SEND_NAVI_BUTTON[b], ACT_ON_PRESS))
        {
          changeScreenContext(SCREEN_CANVAS_MENU);
        }
        break;
      case 1: // Refresh
        if (handleUIButtonPress(&SCREEN_SEND_NAVI_BUTTON[b], ACT_ON_PRESS))
        {
          drawScreenSend(friendListUI.page);
        }
        break;
      case 2: // Sort
              // Not implemented yet.
      case 3: // Up
        if (friendListUI.page > 0)
        {
          if (handleUIButtonPress(&SCREEN_SEND_NAVI_BUTTON[b], ACT_ON_PRESS))
          {
            drawScreenSend(friendListUI.page - 1);
          }
        }
        break;
      case 4: // Down
        if ((friendListUI.page + 1) * SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT < friendListUI.listItems.size())
        {
          if (handleUIButtonPress(&SCREEN_SEND_NAVI_BUTTON[b], ACT_ON_PRESS))
          {
            drawScreenSend(friendListUI.page + 1);
          }
        }
        break;
      default:
        // Not implemented yet.
        break;
      }
      // Draw pressed dropdown.
      // drawScreenCanvasMenu();
    }

    for (uint8_t b = 0; b < SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT; b++)
    {
      if (handleUIButtonPress(&SCREEN_SEND_ADDRESSBOOK_BUTTON[b], ACT_ON_PRESS))
      {
        switch (b)
        {
        case 0:
          drawFriendboxLoadingScreen("Sending...", 0);
          if (networkSendCanvas())
          {
            drawFriendboxLoadingScreen("Sent!", 500);
            delay(500);
          }
          else
          {
            drawFriendboxLoadingScreen("Failed to send.", 1000);
            delay(500);
          }
          drawFramebuffer();
          changeScreenContext(SCREEN_SEND);
          break;
        case 1:
          break;
        case 2:
          break;
        case 3:
          break;
        case 4:
          break;
        default:
          networkSendFramebuffer(b);
          break;
        }
      }
    }
    break;
  case SCREEN_FILE_BROWSER:
    for (uint8_t b = 0; b < SCREEN_FILE_BROWSER_NAVI_BUTTON_COUNT; b++)
    {
      switch (b)
      {
      case 0: // Back
        if (handleUIButtonPress(&SCREEN_FILE_BROWSER_NAVI_BUTTON[b], ACT_ON_PRESS))
        {
          changeScreenContext(SCREEN_CANVAS_MENU);
        }
        break;
      case 1: // Sort
              // Not implemented yet.
      case 2: // Up
        if (fileListUI.page > 0)
        {
          if (handleUIButtonPress(&SCREEN_FILE_BROWSER_NAVI_BUTTON[b], ACT_ON_PRESS))
          {
            drawScreenFileBrowser(fileListUI.page - 1);
          }
        }
        break;
      case 3: // Down
        if ((fileListUI.page + 1) * SCREEN_FILE_BROWSER_FILE_BUTTON_COUNT < fileListUI.listItems.size())
        {
          if (handleUIButtonPress(&SCREEN_FILE_BROWSER_NAVI_BUTTON[b], ACT_ON_PRESS))
          {
            drawScreenFileBrowser(fileListUI.page + 1);
          }
        }
        break;
      default:
        // Not implemented yet.
        break;
      }
      // Draw pressed dropdown.
      // drawScreenCanvasMenu();
    }
    for (uint8_t b = 0; b < SCREEN_FILE_BROWSER_FILE_BUTTON_COUNT; b++)
    {
      if (handleUIButtonPress(&SCREEN_FILE_BROWSER_FILE_BUTTON[b], ACT_ON_PRESS))
      {
        // Use the SAME formula as in drawScreenFileBrowser:
        int fileIndex = b + (fileListUI.page * SCREEN_FILE_BROWSER_FILE_BUTTON_COUNT);

        // Bounds check!
        if (fileIndex < fileListUI.listItems.size())
        {
          const char *filename = fileListUI.listItems[fileIndex].c_str();
          Serial.printf("Select Filename [%d]: %s\n", fileIndex, filename);

          changeScreenContext(SCREEN_CANVAS);
          loadSketchFromSD(filename);
          changeScreenContext(SCREEN_FILE_BROWSER);
          drawScreenFileBrowser(fileListUI.page);
        }
        else
        {
          Serial.printf("ERROR: Index %d out of bounds (size: %d)\n",
                        fileIndex, fileListUI.listItems.size());
        }
      }
    }
    break;
  }
}

/**
 * Handle pressing of hardware button, will implement as hall effect sensor later
 * recheckInput will register another logical press even if button is being held.
 * @param recheckInput Should we register another input in the event the button is still being held?
 */
void handleMenuButton(bool recheckInput)
{
  if (currentScreen == SCREEN_CANVAS || currentScreen == SCREEN_CANVAS_MENU)
  {
    static unsigned long lastPress = 0;
    static unsigned int lastButtonState = 0;
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
        changeScreenContext(SCREEN_CANVAS_MENU);
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
        if (currentScreen != SCREEN_CANVAS)
          changeScreenContext(SCREEN_CANVAS);
      }
    }
  }
}
std::string getUIToolName(draw_tool_id_t tool)
{
  switch (tool)
  {
  case TOOL_PENCIL:
    return "Pencil";
  case TOOL_BRUSH:
    return "Brush";
  case TOOL_FILL:
    return "Fill";
  case TOOL_RAINBOW:
    return "Rainbow";
  case TOOL_DITHER:
    return "Dither";
  case TOOL_STICKER:
    return "Pattern";
  default:
    return "Invalid";
  }
}

std::string getUIContextName(screen_id_t screenContext)
{
  switch (screenContext)
  {
  case SCREEN_CANVAS:
    return "SCREEN_CANVAS";
  case SCREEN_CANVAS_MENU:
    return "SCREEN_CANVAS_MENU";
  case SCREEN_CANVAS_SIZE_SELECT:
    return "SCREEN_CANVAS_SIZE_SELECT";
  case SCREEN_SEND:
    return "SCREEN_SEND";
  case SCREEN_SYSTEM_MESSAGE:
    return "SCREEN_SYSTEM_MESSAGE";
  case SCREEN_RECEIVED:
    return "SCREEN_RECEIVED";
  case SCREEN_WELCOME:
    return "SCREEN_WELCOME";
  case SCREEN_STARTUP:
    return "SCREEN_STARTUP";
  case SCREEN_NETWORK_SETTINGS:
    return "SCREEN_NETWORK_SETTINGS";
  case SCREEN_FILE_BROWSER:
    return "SCREEN_FILE_BROWSER";
  default:
    return "UNKNOWN_SCREEN";
  }
}

std::string getUISubcontextName(dropdown_id_t subcontext)
{
  switch (subcontext)
  {
  case DROPDOWN_NONE:
    return "DROPDOWN_NONE";
  case DROPDOWN_MENU:
    return "DROPDOWN_MENU";
  case DROPDOWN_TOOLS:
    return "DROPDOWN_TOOLS";
  case DROPDOWN_SAVE:
    return "DROPDOWN_SAVE";
  case DROPDOWN_LOAD:
    return "DROPDOWN_LOAD";
  default:
    return "UNKNOWN_DROPDOWN";
  }
}

void changeScreenContext(screen_id_t targetScreen)
{
  Serial.print("Switching Context: ");
  Serial.print(getUIContextName(currentScreen).c_str());
  switch (targetScreen)
  {
  case SCREEN_CANVAS:
    Serial.println(" --> SCREEN_CANVAS");
    if (currentScreen != SCREEN_CANVAS_MENU && currentScreen != SCREEN_CANVAS)
    {
      Serial.println("Deleting from context.");
      currentScreen = SCREEN_CANVAS;
      cleanupUIOutOfContext(true);
    }
    else
    {
      currentScreen = SCREEN_CANVAS;
      cleanupUIOutOfContext(false);
    }
    break;
  case SCREEN_CANVAS_MENU:
    Serial.println(" --> SCREEN_CANVAS_MENU");
    if (currentScreen == SCREEN_CANVAS || currentScreen == SCREEN_CANVAS_MENU)
    {
      currentDropdown = DROPDOWN_NONE;
      currentScreen = SCREEN_CANVAS_MENU;
    }
    else
    {
      currentDropdown = DROPDOWN_NONE;
      currentScreen = SCREEN_CANVAS_MENU;
      cleanupUIOutOfContext(true);
    }
    initUIForScreen(SCREEN_CANVAS_MENU);
    drawScreenCanvasMenu();
    break;
  case SCREEN_SEND:
    Serial.println(" --> SCREEN_SEND");
    if (currentScreen != SCREEN_SEND)
    {
      currentScreen = SCREEN_SEND;
      cleanupUIOutOfContext(true);
      initUIForScreen(SCREEN_SEND);
    }
    currentScreen = SCREEN_SEND;
    drawScreenSend();
    break;
  case SCREEN_FILE_BROWSER:
    Serial.println(" --> SCREEN_FILE_BROWSER");
    if (currentScreen != SCREEN_FILE_BROWSER)
    {
      currentScreen = SCREEN_FILE_BROWSER;
      cleanupUIOutOfContext(true);
      initUIForScreen(SCREEN_FILE_BROWSER);
    }
    currentScreen = SCREEN_FILE_BROWSER;
    fileListUI.page = 0;
    fileListUI.listItems = sdGetFboxFiles();
    drawScreenFileBrowser();
    break;
  case SCREEN_SYSTEM_MESSAGE: // Call this when showing message.
    Serial.println(" --> SCREEN_SYSTEM_MESSAGE");
    currentScreen = SCREEN_SYSTEM_MESSAGE;
    cleanupUIOutOfContext(false);
    break;
  default:

#ifdef FRIENDBOX_DEBUG_MODE
    Serial.println("CRITICAL: Invalid context for drawing canvas menu. Are states correct?");
#endif
  }
}

/** Switch context to loading screen and show while waiting for operations or network activity.
 * @param subtitle Subtitle to show under loading text, can be used to give more context on what we're waiting for.
 * @param holdTimeMs How long should we hold before returning?
 * @param subsubtitle Self-explanatory.
 * @param subsubsubtitle Self-explanatory.
 */
void drawFriendboxLoadingScreen(const char *subtitle, int holdTimeMs, const char *subsubtitle, const char *subsubsubtitle)
{
  lastScreen = currentScreen;                 // Store last screen to return to after showing loading screen.
  changeScreenContext(SCREEN_SYSTEM_MESSAGE); // Change context to system message for loading screen.
  tft.fillScreen(draw_color_palette[currentDrawColorIndex]);
  tft.setTextColor(draw_color_palette_text_color[currentDrawColorIndex], draw_color_palette[currentDrawColorIndex]);
  tft.setTextSize(5);
  tft.drawCenterString("FriendBox", 240, 120);
  tft.setTextSize(3);
  tft.drawCenterString(subtitle, 240, 180);
  if (subsubtitle != "")
  {
    tft.setTextSize(3);
    tft.drawCenterString(subsubtitle, 240, 240);
  }
  if (subsubsubtitle != "")
  {
    tft.setTextSize(2);
    tft.drawCenterString(subsubsubtitle, 240, 270);
  }
  delay(holdTimeMs);               // Wait a moment if specified.
  changeScreenContext(lastScreen); // Return to previous context after showing loading screen.
}

/** Check if UI is already initialized for a given screen context. If it's not, initialize it.
 * @param targetScreen The screen context we want to check for initialization and initialize if not already.
 */
void initUIForScreen(screen_id_t targetScreen)
{
  if (checkIfUIIsInitialized(targetScreen))
  {
    Serial.print("UI already initialized for ");
    Serial.print(getUIContextName(targetScreen).c_str());
    Serial.println(", skipping initialization.");
    return;
  }
  else
  {
    Serial.print("UI not initialized for ");
    Serial.print(getUIContextName(targetScreen).c_str());
    Serial.println(", initializing...");
  }

  switch (targetScreen)
  {
  case SCREEN_CANVAS_MENU:
    // Init Action Buttons (Top)
    for (int col = 0; col < SCREEN_CANVAS_UI_ACTION_BUTTON_COUNT; col++)
    {
      SCREEN_CANVAS_MENU_ACTION_BUTTON[col].x = SCREEN_CANVAS_UI_ACTION_BUTTON_X_POS(col);
      SCREEN_CANVAS_MENU_ACTION_BUTTON[col].y = SCREEN_CANVAS_UI_ACTION_BAR_DIST_FROM_TOP_PX;
      SCREEN_CANVAS_MENU_ACTION_BUTTON[col].w = SCREEN_CANVAS_UI_ACTION_BAR_ITEM_WIDTH_PX;
      SCREEN_CANVAS_MENU_ACTION_BUTTON[col].h = SCREEN_CANVAS_UI_ACTION_BAR_HEIGHT;
      SCREEN_CANVAS_MENU_ACTION_BUTTON[col].fillColor = (int)draw_color_palette[currentDrawColorIndex];
      SCREEN_CANVAS_MENU_ACTION_BUTTON[col].screenContext = SCREEN_CANVAS_MENU;
      SCREEN_CANVAS_MENU_ACTION_BUTTON[col].dropdownContext = DROPDOWN_NONE;
      SCREEN_CANVAS_MENU_ACTION_BUTTON[col].button.initButtonUL(&tft, SCREEN_CANVAS_MENU_ACTION_BUTTON[col].x, SCREEN_CANVAS_MENU_ACTION_BUTTON[col].y, SCREEN_CANVAS_MENU_ACTION_BUTTON[col].w, SCREEN_CANVAS_MENU_ACTION_BUTTON[col].h, TFT_WHITE,
                                                                SCREEN_CANVAS_MENU_ACTION_BUTTON[col].fillColor, (int)draw_color_palette_text_color[currentDrawColorIndex],
                                                                SCREEN_CANVAS_MENU_ACTION_BUTTON_LABEL[col], 2, 2);
      // push back pointer instead of unique object
      uiButtons.push_back(&SCREEN_CANVAS_MENU_ACTION_BUTTON[col]);
    }

    // Init Color Buttons (Bottom)
    for (int col = 0; col < 16; col++)
    {
      SCREEN_CANVAS_MENU_COLOR_BUTTON[col].x = SCREEN_CANVAS_UI_COLOR_BUTTON_X_POS(col);
      SCREEN_CANVAS_MENU_COLOR_BUTTON[col].y = (tft.height() - (SCREEN_CANVAS_UI_COLOR_BAR_ITEM_HEIGHT_PX + SCREEN_CANVAS_UI_COLOR_BAR_DIST_FROM_BOTTOM_PX));
      SCREEN_CANVAS_MENU_COLOR_BUTTON[col].w = SCREEN_CANVAS_UI_COLOR_BAR_ITEM_WIDTH_PX;
      SCREEN_CANVAS_MENU_COLOR_BUTTON[col].h = SCREEN_CANVAS_UI_COLOR_BAR_ITEM_HEIGHT_PX;
      SCREEN_CANVAS_MENU_COLOR_BUTTON[col].fillColor = (int)draw_color_palette[col];
      SCREEN_CANVAS_MENU_COLOR_BUTTON[col].screenContext = SCREEN_CANVAS_MENU;
      SCREEN_CANVAS_MENU_COLOR_BUTTON[col].dropdownContext = DROPDOWN_NONE;
      SCREEN_CANVAS_MENU_COLOR_BUTTON[col].button.initButtonUL(&tft, SCREEN_CANVAS_MENU_COLOR_BUTTON[col].x, SCREEN_CANVAS_MENU_COLOR_BUTTON[col].y, SCREEN_CANVAS_MENU_COLOR_BUTTON[col].w, SCREEN_CANVAS_MENU_COLOR_BUTTON[col].h,
                                                               TFT_WHITE, SCREEN_CANVAS_MENU_COLOR_BUTTON[col].fillColor,
                                                               (int)draw_color_palette_text_color[currentDrawColorIndex], "", 1, 1);
      uiButtons.push_back(&SCREEN_CANVAS_MENU_COLOR_BUTTON[col]);
    }
    // Init Menu Buttons
    for (int col = 0; col < MENU_DROPDOWN_BUTTON_COUNT; col++)
    {
      SCREEN_CANVAS_MENU_MENU_BUTTON[col].x = SCREEN_CANVAS_UI_ACTION_BUTTON_X_POS(0);
      SCREEN_CANVAS_MENU_MENU_BUTTON[col].y = ((col + 1) * (SCREEN_CANVAS_UI_ACTION_BAR_ITEM_HEIGHT_PX + CANVAS_DRAW_MENU_DROPDOWN_DIST_BETWEEN_ITEMS) + SCREEN_CANVAS_UI_ACTION_BAR_DIST_FROM_TOP_PX + SCREEN_CANVAS_UI_ACTION_BAR_ITEM_HEIGHT_PX);
      SCREEN_CANVAS_MENU_MENU_BUTTON[col].w = SCREEN_CANVAS_UI_ACTION_BAR_ITEM_WIDTH_PX;
      SCREEN_CANVAS_MENU_MENU_BUTTON[col].h = SCREEN_CANVAS_UI_ACTION_BAR_ITEM_HEIGHT_PX;
      SCREEN_CANVAS_MENU_MENU_BUTTON[col].fillColor = draw_color_palette[currentDrawColorIndex];
      SCREEN_CANVAS_MENU_MENU_BUTTON[col].screenContext = SCREEN_CANVAS_MENU;
      SCREEN_CANVAS_MENU_MENU_BUTTON[col].dropdownContext = DROPDOWN_MENU;
      SCREEN_CANVAS_MENU_MENU_BUTTON[col].button.initButtonUL(&tft, SCREEN_CANVAS_MENU_MENU_BUTTON[col].x, SCREEN_CANVAS_MENU_MENU_BUTTON[col].y, SCREEN_CANVAS_MENU_MENU_BUTTON[col].w, SCREEN_CANVAS_MENU_MENU_BUTTON[col].h,
                                                              TFT_WHITE, (int)draw_color_palette[currentDrawColorIndex], (int)draw_color_palette_text_color[currentDrawColorIndex],
                                                              SCREEN_CANVAS_MENU_MENU_BUTTON_LABEL[col], 2, 2);
      uiButtons.push_back(&SCREEN_CANVAS_MENU_MENU_BUTTON[col]);
    }
    // Init Tool Buttons
    for (int col = 0; col < TOOL_DROPDOWN_BUTTON_COUNT; col++)
    {
      SCREEN_CANVAS_MENU_TOOL_BUTTON[col].x = SCREEN_CANVAS_UI_ACTION_BUTTON_X_POS(1);
      SCREEN_CANVAS_MENU_TOOL_BUTTON[col].y = ((col + 1) * (SCREEN_CANVAS_UI_ACTION_BAR_ITEM_HEIGHT_PX + CANVAS_DRAW_MENU_DROPDOWN_DIST_BETWEEN_ITEMS) + SCREEN_CANVAS_UI_ACTION_BAR_DIST_FROM_TOP_PX + SCREEN_CANVAS_UI_ACTION_BAR_ITEM_HEIGHT_PX);
      SCREEN_CANVAS_MENU_TOOL_BUTTON[col].w = SCREEN_CANVAS_UI_ACTION_BAR_ITEM_WIDTH_PX;
      SCREEN_CANVAS_MENU_TOOL_BUTTON[col].h = SCREEN_CANVAS_UI_ACTION_BAR_ITEM_HEIGHT_PX;
      SCREEN_CANVAS_MENU_TOOL_BUTTON[col].fillColor = draw_color_palette[currentDrawColorIndex];
      SCREEN_CANVAS_MENU_TOOL_BUTTON[col].screenContext = SCREEN_CANVAS_MENU;
      SCREEN_CANVAS_MENU_TOOL_BUTTON[col].dropdownContext = DROPDOWN_TOOLS;
      SCREEN_CANVAS_MENU_TOOL_BUTTON[col].button.initButtonUL(&tft, SCREEN_CANVAS_MENU_TOOL_BUTTON[col].x, SCREEN_CANVAS_MENU_TOOL_BUTTON[col].y, SCREEN_CANVAS_MENU_TOOL_BUTTON[col].w, SCREEN_CANVAS_MENU_TOOL_BUTTON[col].h,
                                                              TFT_WHITE, (int)draw_color_palette[currentDrawColorIndex], (int)draw_color_palette_text_color[currentDrawColorIndex],
                                                              SCREEN_CANVAS_MENU_TOOL_BUTTON_LABEL[col], 2, 2);
      uiButtons.push_back(&SCREEN_CANVAS_MENU_TOOL_BUTTON[col]);
    }

    // Init Tool Settings Buttons (change size n stuff)
    for (int col = 0; col < SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON_COUNT; col++)
    {
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[col].x = SCREEN_CANVAS_UI_ACTION_BUTTON_X_POS(2);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[col].y = ((col + 1) * (SCREEN_CANVAS_UI_ACTION_BAR_ITEM_HEIGHT_PX + CANVAS_DRAW_MENU_DROPDOWN_DIST_BETWEEN_ITEMS) + SCREEN_CANVAS_UI_ACTION_BAR_DIST_FROM_TOP_PX + SCREEN_CANVAS_UI_ACTION_BAR_ITEM_HEIGHT_PX);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[col].w = SCREEN_CANVAS_UI_ACTION_BAR_ITEM_WIDTH_PX;
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[col].h = SCREEN_CANVAS_UI_ACTION_BAR_ITEM_HEIGHT_PX;
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[col].fillColor = draw_color_palette[currentDrawColorIndex];
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[col].screenContext = SCREEN_CANVAS_MENU;
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[col].dropdownContext = DROPDOWN_TOOLS;
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[col].button.initButtonUL(&tft, SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[col].x, SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[col].y, SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[col].w, SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[col].h,
                                                                       TFT_WHITE, (int)draw_color_palette[currentDrawColorIndex], (int)draw_color_palette_text_color[currentDrawColorIndex],
                                                                       "---", 2, 2);
      uiButtons.push_back(&SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[col]);
    }

    // Init Save Buttons
    for (int col = 0; col < SLOT_DROPDOWN_BUTTON_COUNT; col++)
    {
      SCREEN_CANVAS_MENU_SAVE_BUTTON[col].x = SCREEN_CANVAS_UI_ACTION_BUTTON_X_POS(2);
      SCREEN_CANVAS_MENU_SAVE_BUTTON[col].y = ((col + 1) * (SCREEN_CANVAS_UI_ACTION_BAR_ITEM_HEIGHT_PX + CANVAS_DRAW_MENU_DROPDOWN_DIST_BETWEEN_ITEMS) + SCREEN_CANVAS_UI_ACTION_BAR_DIST_FROM_TOP_PX + SCREEN_CANVAS_UI_ACTION_BAR_ITEM_HEIGHT_PX);
      SCREEN_CANVAS_MENU_SAVE_BUTTON[col].w = SCREEN_CANVAS_UI_ACTION_BAR_ITEM_WIDTH_PX;
      SCREEN_CANVAS_MENU_SAVE_BUTTON[col].h = SCREEN_CANVAS_UI_ACTION_BAR_ITEM_HEIGHT_PX;
      SCREEN_CANVAS_MENU_SAVE_BUTTON[col].fillColor = draw_color_palette[currentDrawColorIndex];
      SCREEN_CANVAS_MENU_SAVE_BUTTON[col].screenContext = SCREEN_CANVAS_MENU;
      SCREEN_CANVAS_MENU_SAVE_BUTTON[col].dropdownContext = DROPDOWN_SAVE;
      SCREEN_CANVAS_MENU_SAVE_BUTTON[col].button.initButtonUL(&tft, SCREEN_CANVAS_MENU_SAVE_BUTTON[col].x, SCREEN_CANVAS_MENU_SAVE_BUTTON[col].y, SCREEN_CANVAS_MENU_SAVE_BUTTON[col].w, SCREEN_CANVAS_MENU_SAVE_BUTTON[col].h,
                                                              TFT_WHITE, (int)draw_color_palette[currentDrawColorIndex], (int)draw_color_palette_text_color[currentDrawColorIndex],
                                                              SCREEN_CANVAS_MENU_SAVE_BUTTON_LABEL[col], 2, 2);
      uiButtons.push_back(&SCREEN_CANVAS_MENU_SAVE_BUTTON[col]);
    }
    // Init Load Buttons
    for (int col = 0; col < SLOT_DROPDOWN_BUTTON_COUNT; col++)
    {
      SCREEN_CANVAS_MENU_LOAD_BUTTON[col].x = SCREEN_CANVAS_UI_ACTION_BUTTON_X_POS(3);
      SCREEN_CANVAS_MENU_LOAD_BUTTON[col].y = ((col + 1) * (SCREEN_CANVAS_UI_ACTION_BAR_ITEM_HEIGHT_PX + CANVAS_DRAW_MENU_DROPDOWN_DIST_BETWEEN_ITEMS) + SCREEN_CANVAS_UI_ACTION_BAR_DIST_FROM_TOP_PX + SCREEN_CANVAS_UI_ACTION_BAR_ITEM_HEIGHT_PX);
      SCREEN_CANVAS_MENU_LOAD_BUTTON[col].w = SCREEN_CANVAS_UI_ACTION_BAR_ITEM_WIDTH_PX;
      SCREEN_CANVAS_MENU_LOAD_BUTTON[col].h = SCREEN_CANVAS_UI_ACTION_BAR_ITEM_HEIGHT_PX;
      SCREEN_CANVAS_MENU_LOAD_BUTTON[col].fillColor = draw_color_palette[currentDrawColorIndex];
      SCREEN_CANVAS_MENU_LOAD_BUTTON[col].screenContext = SCREEN_CANVAS_MENU;
      SCREEN_CANVAS_MENU_LOAD_BUTTON[col].dropdownContext = DROPDOWN_LOAD;
      SCREEN_CANVAS_MENU_LOAD_BUTTON[col].button.initButtonUL(&tft, SCREEN_CANVAS_MENU_LOAD_BUTTON[col].x, SCREEN_CANVAS_MENU_LOAD_BUTTON[col].y, SCREEN_CANVAS_MENU_LOAD_BUTTON[col].w, SCREEN_CANVAS_MENU_LOAD_BUTTON[col].h,
                                                              TFT_WHITE, (int)draw_color_palette[currentDrawColorIndex], (int)draw_color_palette_text_color[currentDrawColorIndex],
                                                              SCREEN_CANVAS_MENU_LOAD_BUTTON_LABEL[col], 2, 2);
      uiButtons.push_back(&SCREEN_CANVAS_MENU_LOAD_BUTTON[col]);
    }
    break;
  case SCREEN_SEND:
    // Init Address Buttons
    for (int col = 0; col < SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT; col++)
    {
      SCREEN_SEND_ADDRESSBOOK_BUTTON[col].x = 10;
      SCREEN_SEND_ADDRESSBOOK_BUTTON[col].y = 60 * col + 15;
      SCREEN_SEND_ADDRESSBOOK_BUTTON[col].w = 300;
      SCREEN_SEND_ADDRESSBOOK_BUTTON[col].h = 50;
      SCREEN_SEND_ADDRESSBOOK_BUTTON[col].fillColor = (int)draw_color_palette[currentDrawColorIndex];
      SCREEN_SEND_ADDRESSBOOK_BUTTON[col].screenContext = SCREEN_SEND;
      SCREEN_SEND_ADDRESSBOOK_BUTTON[col].dropdownContext = DROPDOWN_NONE;
      SCREEN_SEND_ADDRESSBOOK_BUTTON[col].button.initButtonUL(&tft, SCREEN_SEND_ADDRESSBOOK_BUTTON[col].x, SCREEN_SEND_ADDRESSBOOK_BUTTON[col].y,
                                                              SCREEN_SEND_ADDRESSBOOK_BUTTON[col].w, SCREEN_SEND_ADDRESSBOOK_BUTTON[col].h, TFT_WHITE,
                                                              SCREEN_SEND_ADDRESSBOOK_BUTTON[col].fillColor, (int)draw_color_palette_text_color[currentDrawColorIndex],
                                                              "Working...", 2, 2);
      // push back pointer instead of unique object
      uiButtons.push_back(&SCREEN_SEND_ADDRESSBOOK_BUTTON[col]);
    }

    // Init Navigation Buttons
    for (int col = 0; col < SCREEN_SEND_NAVI_BUTTON_COUNT; col++)
    {
      SCREEN_SEND_NAVI_BUTTON[col].x = 350;
      SCREEN_SEND_NAVI_BUTTON[col].y = 60 * col + 15;
      SCREEN_SEND_NAVI_BUTTON[col].w = 100;
      SCREEN_SEND_NAVI_BUTTON[col].h = 50;
      SCREEN_SEND_NAVI_BUTTON[col].fillColor = (int)draw_color_palette[currentDrawColorIndex];
      SCREEN_SEND_NAVI_BUTTON[col].screenContext = SCREEN_SEND;
      SCREEN_SEND_NAVI_BUTTON[col].dropdownContext = DROPDOWN_NONE;
      SCREEN_SEND_NAVI_BUTTON[col].button.initButtonUL(&tft, SCREEN_SEND_NAVI_BUTTON[col].x, SCREEN_SEND_NAVI_BUTTON[col].y,
                                                       SCREEN_SEND_NAVI_BUTTON[col].w, SCREEN_SEND_NAVI_BUTTON[col].h, TFT_WHITE,
                                                       SCREEN_SEND_NAVI_BUTTON[col].fillColor, (int)draw_color_palette_text_color[currentDrawColorIndex],
                                                       SCREEN_SEND_NAVI_BUTTON_LABEL[col], 2, 2);
      // push back pointer instead of unique object
      uiButtons.push_back(&SCREEN_SEND_NAVI_BUTTON[col]);
    }
    break;
  case SCREEN_FILE_BROWSER:
    // Init File Buttons
    for (int col = 0; col < SCREEN_FILE_BROWSER_FILE_BUTTON_COUNT; col++)
    {
      SCREEN_FILE_BROWSER_FILE_BUTTON[col].x = 10;
      SCREEN_FILE_BROWSER_FILE_BUTTON[col].y = 60 * col + 15;
      SCREEN_FILE_BROWSER_FILE_BUTTON[col].w = 300;
      SCREEN_FILE_BROWSER_FILE_BUTTON[col].h = 50;
      SCREEN_FILE_BROWSER_FILE_BUTTON[col].fillColor = (int)draw_color_palette[currentDrawColorIndex];
      SCREEN_FILE_BROWSER_FILE_BUTTON[col].screenContext = SCREEN_FILE_BROWSER;
      SCREEN_FILE_BROWSER_FILE_BUTTON[col].dropdownContext = DROPDOWN_NONE;
      SCREEN_FILE_BROWSER_FILE_BUTTON[col].button.initButtonUL(&tft, SCREEN_FILE_BROWSER_FILE_BUTTON[col].x, SCREEN_FILE_BROWSER_FILE_BUTTON[col].y,
                                                               SCREEN_FILE_BROWSER_FILE_BUTTON[col].w, SCREEN_FILE_BROWSER_FILE_BUTTON[col].h, TFT_WHITE,
                                                               SCREEN_FILE_BROWSER_FILE_BUTTON[col].fillColor, (int)draw_color_palette_text_color[currentDrawColorIndex],
                                                               "File Name", 2, 2);
      // push back pointer instead of unique object
      uiButtons.push_back(&SCREEN_FILE_BROWSER_FILE_BUTTON[col]);
    }

    // Init File Navigation Buttons
    for (int col = 0; col < SCREEN_FILE_BROWSER_NAVI_BUTTON_COUNT; col++)
    {
      SCREEN_FILE_BROWSER_NAVI_BUTTON[col].x = 350;
      SCREEN_FILE_BROWSER_NAVI_BUTTON[col].y = 60 * col + 15;
      SCREEN_FILE_BROWSER_NAVI_BUTTON[col].w = 100;
      SCREEN_FILE_BROWSER_NAVI_BUTTON[col].h = 50;
      SCREEN_FILE_BROWSER_NAVI_BUTTON[col].fillColor = (int)draw_color_palette[currentDrawColorIndex];
      SCREEN_FILE_BROWSER_NAVI_BUTTON[col].screenContext = SCREEN_FILE_BROWSER;
      SCREEN_FILE_BROWSER_NAVI_BUTTON[col].dropdownContext = DROPDOWN_NONE;
      SCREEN_FILE_BROWSER_NAVI_BUTTON[col].button.initButtonUL(&tft, SCREEN_FILE_BROWSER_NAVI_BUTTON[col].x, SCREEN_FILE_BROWSER_NAVI_BUTTON[col].y,
                                                               SCREEN_FILE_BROWSER_NAVI_BUTTON[col].w, SCREEN_FILE_BROWSER_NAVI_BUTTON[col].h, TFT_WHITE,
                                                               SCREEN_FILE_BROWSER_NAVI_BUTTON[col].fillColor, (int)draw_color_palette_text_color[currentDrawColorIndex],
                                                               SCREEN_FILE_BROWSER_NAVI_BUTTON_LABEL[col], 2, 2);
      // push back pointer instead of unique object
      uiButtons.push_back(&SCREEN_FILE_BROWSER_NAVI_BUTTON[col]);
    }
    break;
  default:
    break;
  }
}

void drawScreenFileBrowser(int page)
{
  Serial.print("Drawing SCREEN_FILE_BROWSER on page ");
  Serial.println(page);
  if (!SCREEN_FILE_BROWSER_FILE_BUTTON[0].isDrawn || fileListUI.page != page)
  {
    for (int col = 0; col < SCREEN_FILE_BROWSER_FILE_BUTTON_COUNT; col++)
    {
      // Always erase the button area
      drawFramebuffer(SCREEN_FILE_BROWSER_FILE_BUTTON[col].x,
                      SCREEN_FILE_BROWSER_FILE_BUTTON[col].y,
                      SCREEN_FILE_BROWSER_FILE_BUTTON[col].w,
                      SCREEN_FILE_BROWSER_FILE_BUTTON[col].h);

      int fileIndex = col + (page * SCREEN_FILE_BROWSER_FILE_BUTTON_COUNT);

      // Check if we have a file for this button
      if (fileIndex < fileListUI.listItems.size())
      {
        // Draw the button with file name
        SCREEN_FILE_BROWSER_FILE_BUTTON[col].fillColor = draw_color_palette[currentDrawColorIndex];
        SCREEN_FILE_BROWSER_FILE_BUTTON[col].button.setTextColor(draw_color_palette_text_color[currentDrawColorIndex]);
        SCREEN_FILE_BROWSER_FILE_BUTTON[col].button.setFillColor(draw_color_palette[currentDrawColorIndex]);
        SCREEN_FILE_BROWSER_FILE_BUTTON[col].button.drawButton(false, fileListUI.listItems[fileIndex].c_str());
        SCREEN_FILE_BROWSER_FILE_BUTTON[col].isDrawn = true;
      }
      else
      {
        // No friend for this slot, leave erased
        SCREEN_FILE_BROWSER_FILE_BUTTON[col].isDrawn = false;
      }
    }
  }
  if (!SCREEN_FILE_BROWSER_NAVI_BUTTON[0].isDrawn || fileListUI.page != page)
  {
    for (int col = 0; col < SCREEN_FILE_BROWSER_NAVI_BUTTON_COUNT; col++)
    {
      switch (col)
      {
      case 0: // Back
      case 1: // Sort
        SCREEN_FILE_BROWSER_NAVI_BUTTON[col].fillColor = draw_color_palette[currentDrawColorIndex];
        SCREEN_FILE_BROWSER_NAVI_BUTTON[col].button.setTextColor(draw_color_palette_text_color[currentDrawColorIndex]);
        SCREEN_FILE_BROWSER_NAVI_BUTTON[col].button.setFillColor(draw_color_palette[currentDrawColorIndex]);
        SCREEN_FILE_BROWSER_NAVI_BUTTON[col].button.drawButton();
        SCREEN_FILE_BROWSER_NAVI_BUTTON[col].isDrawn = true;
        break;
      case 2: // Up
        Serial.println("Drawing over previous up button.");
        drawFramebuffer(SCREEN_FILE_BROWSER_NAVI_BUTTON[col].x,
                        SCREEN_FILE_BROWSER_NAVI_BUTTON[col].y,
                        SCREEN_FILE_BROWSER_NAVI_BUTTON[col].w,
                        SCREEN_FILE_BROWSER_NAVI_BUTTON[col].h);
        if (page > 0)
        {
          Serial.print("Drawing up buttons because page is ");
          Serial.println(page);
          SCREEN_FILE_BROWSER_NAVI_BUTTON[col].fillColor = draw_color_palette[currentDrawColorIndex];
          SCREEN_FILE_BROWSER_NAVI_BUTTON[col].button.setTextColor(draw_color_palette_text_color[currentDrawColorIndex]);
          SCREEN_FILE_BROWSER_NAVI_BUTTON[col].button.setFillColor(draw_color_palette[currentDrawColorIndex]);
          SCREEN_FILE_BROWSER_NAVI_BUTTON[col].button.drawButton();
          SCREEN_FILE_BROWSER_NAVI_BUTTON[col].isDrawn = true;
        }
        else
        {
          SCREEN_FILE_BROWSER_NAVI_BUTTON[col].isDrawn = false;
        }
        break;
      case 3: // Down
        drawFramebuffer(SCREEN_FILE_BROWSER_NAVI_BUTTON[col].x,
                        SCREEN_FILE_BROWSER_NAVI_BUTTON[col].y,
                        SCREEN_FILE_BROWSER_NAVI_BUTTON[col].w,
                        SCREEN_FILE_BROWSER_NAVI_BUTTON[col].h);
        if ((page + 1) * 5 < fileListUI.listItems.size())
        {
          Serial.print("Drawing down button because ");
          Serial.print((page + 1) * SCREEN_FILE_BROWSER_FILE_BUTTON_COUNT);
          Serial.print(" < ");
          Serial.println(fileListUI.listItems.size());
          SCREEN_FILE_BROWSER_NAVI_BUTTON[col].fillColor = draw_color_palette[currentDrawColorIndex];
          SCREEN_FILE_BROWSER_NAVI_BUTTON[col].button.setTextColor(draw_color_palette_text_color[currentDrawColorIndex]);
          SCREEN_FILE_BROWSER_NAVI_BUTTON[col].button.setFillColor(draw_color_palette[currentDrawColorIndex]);
          SCREEN_FILE_BROWSER_NAVI_BUTTON[col].button.drawButton();
          SCREEN_FILE_BROWSER_NAVI_BUTTON[col].isDrawn = true;
        }
        else
        {
          SCREEN_FILE_BROWSER_NAVI_BUTTON[col].isDrawn = false;
        }
        break;
      default:
        break;
      }
    }
  }
  fileListUI.page = page;
}

/**
 * Draw
 * @param page Starting from zero, show which "page" of friends we're showing in the address book. Each page shows 5 friends, so page 0 shows friends 0-4, page 1 shows friends 5-9, etc.
 */
void drawScreenSend(int page)
{
  Serial.print("Drawing SCREEN_SEND on page ");
  Serial.println(page);
  friendListUI.listItems = networkGetFriends();
  if (!SCREEN_SEND_ADDRESSBOOK_BUTTON[0].isDrawn || friendListUI.page != page)
  {
    for (int col = 0; col < SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT; col++)
    {
      // Always erase the button area
      drawFramebuffer(SCREEN_SEND_ADDRESSBOOK_BUTTON[col].x,
                      SCREEN_SEND_ADDRESSBOOK_BUTTON[col].y,
                      SCREEN_SEND_ADDRESSBOOK_BUTTON[col].w,
                      SCREEN_SEND_ADDRESSBOOK_BUTTON[col].h);

      int friendIndex = col + (page * SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT);

      // Check if we have a friend for this button
      if (friendIndex < friendListUI.listItems.size())
      {
        // Draw the button with friend name
        SCREEN_SEND_ADDRESSBOOK_BUTTON[col].fillColor = draw_color_palette[currentDrawColorIndex];
        SCREEN_SEND_ADDRESSBOOK_BUTTON[col].button.setTextColor(draw_color_palette_text_color[currentDrawColorIndex]);
        SCREEN_SEND_ADDRESSBOOK_BUTTON[col].button.setFillColor(draw_color_palette[currentDrawColorIndex]);
        SCREEN_SEND_ADDRESSBOOK_BUTTON[col].button.drawButton(false, friendListUI.listItems[friendIndex].c_str());
        SCREEN_SEND_ADDRESSBOOK_BUTTON[col].isDrawn = true;
      }
      else
      {
        // No friend for this slot, leave erased
        SCREEN_SEND_ADDRESSBOOK_BUTTON[col].isDrawn = false;
      }
    }
  }
  if (!SCREEN_SEND_NAVI_BUTTON[0].isDrawn || friendListUI.page != page)
  {
    for (int col = 0; col < SCREEN_SEND_NAVI_BUTTON_COUNT; col++)
    {
      switch (col)
      {
      case 0: // Canvas
      case 1: // Refresh
      case 2: // Sort
        SCREEN_SEND_NAVI_BUTTON[col].fillColor = draw_color_palette[currentDrawColorIndex];
        SCREEN_SEND_NAVI_BUTTON[col].button.setTextColor(draw_color_palette_text_color[currentDrawColorIndex]);
        SCREEN_SEND_NAVI_BUTTON[col].button.setFillColor(draw_color_palette[currentDrawColorIndex]);
        SCREEN_SEND_NAVI_BUTTON[col].button.drawButton();
        SCREEN_SEND_NAVI_BUTTON[col].isDrawn = true;
        break;
      case 3: // Up
        Serial.println("Drawing over previous up button.");
        drawFramebuffer(SCREEN_SEND_NAVI_BUTTON[col].x,
                        SCREEN_SEND_NAVI_BUTTON[col].y,
                        SCREEN_SEND_NAVI_BUTTON[col].w,
                        SCREEN_SEND_NAVI_BUTTON[col].h);
        if (page > 0)
        {
          Serial.print("Drawing up buttons because page is ");
          Serial.println(page);
          SCREEN_SEND_NAVI_BUTTON[col].fillColor = draw_color_palette[currentDrawColorIndex];
          SCREEN_SEND_NAVI_BUTTON[col].button.setTextColor(draw_color_palette_text_color[currentDrawColorIndex]);
          SCREEN_SEND_NAVI_BUTTON[col].button.setFillColor(draw_color_palette[currentDrawColorIndex]);
          SCREEN_SEND_NAVI_BUTTON[col].button.drawButton();
          SCREEN_SEND_NAVI_BUTTON[col].isDrawn = true;
        }
        else
        {
          SCREEN_SEND_NAVI_BUTTON[col].isDrawn = false;
        }
        break;
      case 4: // Down
        drawFramebuffer(SCREEN_SEND_NAVI_BUTTON[col].x,
                        SCREEN_SEND_NAVI_BUTTON[col].y,
                        SCREEN_SEND_NAVI_BUTTON[col].w,
                        SCREEN_SEND_NAVI_BUTTON[col].h);
        if ((page + 1) * 5 < friendListUI.listItems.size())
        {
          SCREEN_SEND_NAVI_BUTTON[col].fillColor = draw_color_palette[currentDrawColorIndex];
          SCREEN_SEND_NAVI_BUTTON[col].button.setTextColor(draw_color_palette_text_color[currentDrawColorIndex]);
          SCREEN_SEND_NAVI_BUTTON[col].button.setFillColor(draw_color_palette[currentDrawColorIndex]);
          SCREEN_SEND_NAVI_BUTTON[col].button.drawButton();
          SCREEN_SEND_NAVI_BUTTON[col].isDrawn = true;
        }
        else
        {
          SCREEN_SEND_NAVI_BUTTON[col].isDrawn = false;
        }
        break;
      default:
        break;
      }
    }
  }
  friendListUI.page = page;
}

void animateUIElement(UIButton *elements[], ui_anim_mode_id_t animation, int timeInMS)
{
  switch (animation)
  {
    // to be imnplemented
  }
}

void drawScreenCanvasMenu()
{
  Serial.println("Drawing SCREEN_CANVAS_MENU");
  // tft.fillRoundRect(200 - 1, 200 - 1, CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX + 2, CANVAS_DRAW_MENU_TOP_BAR_HEIGHT_PX + 2, 7, TFT_WHITE);
  // tft.fillRoundRect(200, 200, CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX, CANVAS_DRAW_MENU_TOP_BAR_HEIGHT_PX, 7, (int)draw_color_palette[currentDrawColorIndex]);
  //  Draw Action Bar if Color Doesn't Match or Not Drawn
  SCREEN_CANVAS_MENU_ACTION_BUTTON[1].button.setLabelText(getUIToolName(currentTool).c_str());
  if (SCREEN_CANVAS_MENU_ACTION_BUTTON[0].fillColor != draw_color_palette[currentDrawColorIndex] || !SCREEN_CANVAS_MENU_ACTION_BUTTON[0].isDrawn)
  {
    for (int col = 0; col < SCREEN_CANVAS_UI_ACTION_BUTTON_COUNT; col++)
    {
      SCREEN_CANVAS_MENU_ACTION_BUTTON[col].fillColor = draw_color_palette[currentDrawColorIndex];
      SCREEN_CANVAS_MENU_ACTION_BUTTON[col].button.setTextColor(draw_color_palette_text_color[currentDrawColorIndex]);
      SCREEN_CANVAS_MENU_ACTION_BUTTON[col].button.setFillColor(draw_color_palette[currentDrawColorIndex]);
      /*for (int y = -CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX; y < CANVAS_DRAW_MENU_TOP_BAR_DIST_FROM_TOP_PX; y++)
      {
        SCREEN_CANVAS_MENU_ACTION_BUTTON[col].button.initButtonUL(&tft, SCREEN_CANVAS_MENU_ACTION_BUTTON[col].x, y, SCREEN_CANVAS_MENU_ACTION_BUTTON[col].w, SCREEN_CANVAS_MENU_ACTION_BUTTON[col].h, TFT_WHITE,
                                                                  SCREEN_CANVAS_MENU_ACTION_BUTTON[col].fillColor, (int)draw_color_palette_text_color[currentDrawColorIndex],
                                                                  SCREEN_CANVAS_MENU_ACTION_BUTTON_LABEL[col], 3, 3);
        SCREEN_CANVAS_MENU_ACTION_BUTTON[col].button.drawButton();
        // delay(10); // Adjust delay for smoother/faster animation
      }*/
      SCREEN_CANVAS_MENU_ACTION_BUTTON[col].button.drawButton();
      SCREEN_CANVAS_MENU_ACTION_BUTTON[col].isDrawn = true;
    }
  }

  // Draw Color Bar if not drawn.
  if (!SCREEN_CANVAS_MENU_COLOR_BUTTON[0].isDrawn)
    for (int col = 0; col < 16; col++)
    {
      SCREEN_CANVAS_MENU_COLOR_BUTTON[col].button.drawButton();
      SCREEN_CANVAS_MENU_COLOR_BUTTON[col].isDrawn = true;
    }
  switch (currentDropdown)
  {
  case DROPDOWN_NONE:
    break;
  case DROPDOWN_MENU:
    for (int col = 0; col < MENU_DROPDOWN_BUTTON_COUNT; col++)
    {
      SCREEN_CANVAS_MENU_MENU_BUTTON[col].button.setFillColor(draw_color_palette[currentDrawColorIndex]);
      SCREEN_CANVAS_MENU_MENU_BUTTON[col].button.setTextColor(draw_color_palette_text_color[currentDrawColorIndex]);
      SCREEN_CANVAS_MENU_MENU_BUTTON[col].button.drawButton();
      SCREEN_CANVAS_MENU_MENU_BUTTON[col].isDrawn = true;
    }
    break;
  case DROPDOWN_TOOLS:
    for (int col = 0; col < TOOL_DROPDOWN_BUTTON_COUNT; col++)
    {
      SCREEN_CANVAS_MENU_TOOL_BUTTON[col].button.setFillColor(draw_color_palette[currentDrawColorIndex]);
      SCREEN_CANVAS_MENU_TOOL_BUTTON[col].button.setTextColor(draw_color_palette_text_color[currentDrawColorIndex]);
      if (col == currentTool)
      {
        SCREEN_CANVAS_MENU_TOOL_BUTTON[col].button.drawButton(true);
      }
      else
      {
        SCREEN_CANVAS_MENU_TOOL_BUTTON[col].button.drawButton();
      }
      SCREEN_CANVAS_MENU_TOOL_BUTTON[col].isDrawn = true;
    }
    for (int col = 0; col < SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON_COUNT; col++)
    {
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[col].button.setFillColor(draw_color_palette[currentDrawColorIndex]);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[col].button.setTextColor(draw_color_palette_text_color[currentDrawColorIndex]);
    }
    char sizeStatus[20];
    switch (currentTool)
    {
    case TOOL_PENCIL:
    case TOOL_BRUSH:
    case TOOL_RAINBOW:
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[0].button.setLabelText("- Size");
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[0].button.drawButton(false);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[0].isDrawn = true;
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[1].button.setLabelText("+ Size");
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[1].button.drawButton(false);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[1].isDrawn = true;
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[2].button.setLabelText("");
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[2].button.drawButton(false);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[2].isDrawn = true;
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[3].button.setLabelText("");
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[3].button.drawButton(false);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[3].isDrawn = true;
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[4].button.setLabelText("");
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[4].button.drawButton(false);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[4].isDrawn = true;
      snprintf(sizeStatus, sizeof(sizeStatus), "Size: %d", currentBrushRadius);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[5].button.setLabelText(sizeStatus);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[5].button.drawButton(false);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[5].isDrawn = true;
      break;
    case TOOL_FILL:
      break;
    case TOOL_DITHER:
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[0].button.setLabelText("- Size");
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[0].button.drawButton(false);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[0].isDrawn = true;
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[1].button.setLabelText("+ Size");
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[1].button.drawButton(false);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[1].isDrawn = true;
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[2].button.setLabelText("Draw Odd");
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[2].button.drawButton(false);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[2].isDrawn = true;
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[3].button.setLabelText("Draw Even");
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[3].button.drawButton(false);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[3].isDrawn = true;
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[4].button.setLabelText("Curr: E");
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[4].button.drawButton(false);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[4].isDrawn = true;
      snprintf(sizeStatus, sizeof(sizeStatus), "Size: %d", currentBrushRadius);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[5].button.setLabelText(sizeStatus);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[5].button.drawButton(false);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[5].isDrawn = true;
      break;
    case TOOL_STICKER: // actually pattern for now, but we can use downsampling algorithm to do this!
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[0].button.setLabelText("Size 1x");
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[0].button.drawButton(false);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[0].isDrawn = true;
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[1].button.setLabelText("Size 2x");
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[1].button.drawButton(false);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[1].isDrawn = true;
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[2].button.setLabelText("Size 3x");
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[2].button.drawButton(false);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[2].isDrawn = true;
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[2].button.setLabelText("Select");
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[2].button.drawButton(false);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[2].isDrawn = true;
            SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[3].button.setLabelText("");
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[3].button.drawButton(false);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[3].isDrawn = true;
            SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[4].button.setLabelText("");
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[4].button.drawButton(false);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[4].isDrawn = true;
            SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[5].button.setLabelText("");
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[5].button.drawButton(false);
      SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[5].isDrawn = true;
      break;
    default:
      break;
    }
    break;
  case DROPDOWN_SAVE:
    for (int col = 0; col < SLOT_DROPDOWN_BUTTON_COUNT; col++)
    {
      SCREEN_CANVAS_MENU_SAVE_BUTTON[col].button.setFillColor(draw_color_palette[currentDrawColorIndex]);
      SCREEN_CANVAS_MENU_SAVE_BUTTON[col].button.setTextColor(draw_color_palette_text_color[currentDrawColorIndex]);
      SCREEN_CANVAS_MENU_SAVE_BUTTON[col].button.drawButton();
      SCREEN_CANVAS_MENU_SAVE_BUTTON[col].isDrawn = true;
    }
    break;
  case DROPDOWN_LOAD:
    for (int col = 0; col < SLOT_DROPDOWN_BUTTON_COUNT; col++)
    {
      SCREEN_CANVAS_MENU_LOAD_BUTTON[col].button.setFillColor(draw_color_palette[currentDrawColorIndex]);
      SCREEN_CANVAS_MENU_LOAD_BUTTON[col].button.setTextColor(draw_color_palette_text_color[currentDrawColorIndex]);
      SCREEN_CANVAS_MENU_LOAD_BUTTON[col].button.drawButton();
      SCREEN_CANVAS_MENU_LOAD_BUTTON[col].isDrawn = true;
    }
    break;
  }
}

bool drawSketchPreview(const char *filepath, int x, int y, int scaleDown, bool drawBorder)
{
  // scale = 2 means 480x320 → 240x160
  // scale = 3 means 480x320 → 160x107
  // scale = 4 means 480x320 → 120x80

  File f = SD.open(filepath, FILE_READ);
  if (!f)
    return false;

  const int SOURCE_WIDTH = 480;
  const int SOURCE_HEIGHT = 320;
  int w = SOURCE_WIDTH / scaleDown;
  int h = SOURCE_HEIGHT / scaleDown;

  tft.startWrite();

  // Read file in strips to save memory
  const int STRIP_HEIGHT = 16; // Process 16 source rows at a time
  uint8_t *stripBuffer = (uint8_t *)malloc((SOURCE_WIDTH * STRIP_HEIGHT) / 2);
  if (!stripBuffer)
  {
    f.close();
    return false;
  }

  for (int stripY = 0; stripY < SOURCE_HEIGHT; stripY += STRIP_HEIGHT)
  {
    // Read strip from file
    size_t bytesToRead = (SOURCE_WIDTH * STRIP_HEIGHT) / 2;
    f.read(stripBuffer, bytesToRead);

    // Process this strip
    for (int localY = 0; localY < STRIP_HEIGHT; localY += scaleDown)
    {
      int srcY = stripY + localY;
      int destY = srcY / scaleDown;

      if (destY >= h)
        break;

      for (int srcX = 0; srcX < SOURCE_WIDTH; srcX += scaleDown)
      {
        int destX = srcX / scaleDown;

        // Just sample top-left pixel of each block (no averaging)
        int pixelIndex = localY * SOURCE_WIDTH + srcX;
        int byteIndex = pixelIndex >> 1;
        uint8_t byte = stripBuffer[byteIndex];

        uint8_t colorIndex;
        if (pixelIndex & 1)
        {
          colorIndex = byte & 0x0F;
        }
        else
        {
          colorIndex = (byte >> 4) & 0x0F;
        }

        tft.drawPixel(x + destX, y + destY, draw_color_palette[colorIndex]);
      }
    }
  }

  tft.endWrite();
  free(stripBuffer);
  f.close();

  if (drawBorder)
  {
    tft.drawRect(x - 1, y - 1, w + 2, h + 2, TFT_WHITE);
  }

  return true;
}

/** Loop through current UI elements to see if any exist belonging to the target context.
 * @param targetScreen Which screen context are we checking for?
 * @return bool True if we find an element in the target context, false if we loop through all elements without finding one.
 */
bool checkIfUIIsInitialized(screen_id_t targetScreen)
{
  for (int i = 0; i < uiButtons.size(); i++)
  {
    if (uiButtons[i]->screenContext == targetScreen)
    {
      return true;
    }
  }
  return false;
}

/**
 * Search all registered UI buttons, determine if they are in context, and if not, write over them with canvas.
 * @param removeFromContext On top of redrawing over element, should we also remove it from the UI elements vector?
 */
void cleanupUIOutOfContext(bool removeFromContext)
{
  // Serial.print("UI Elements in Context:");
  // Serial.println(uiButtons.size());
  for (int i = uiButtons.size() - 1; i >= 0; i--)
  {
    // Serial.print("CHECKING: ");
    // Serial.print(getUIContextName(uiButtons[i]->screenContext).c_str());
    // Serial.print(" - ");
    // Serial.print(getUISubcontextName(uiButtons[i]->dropdownContext).c_str());

    if (currentScreen != uiButtons[i]->screenContext ||
        (currentDropdown != uiButtons[i]->dropdownContext && uiButtons[i]->dropdownContext != DROPDOWN_NONE))
    {
      // Overwrite drawn elements with canvas.
      if (uiButtons[i]->isDrawn)
      {
        // Serial.print(" - DRAWING OVER");
        drawFramebuffer(uiButtons[i]->x, uiButtons[i]->y, uiButtons[i]->w, uiButtons[i]->h);
        uiButtons[i]->isDrawn = false;
      }

      // Remove from vector if specified, we're counting backwards to avoid issues with shifting indices.
      if (removeFromContext)
      {
        // Serial.print(" AND REMOVING");
        uiButtons[i]->isDrawn = false;
        uiButtons.erase(uiButtons.begin() + i);
      }
    }
    // Serial.println("");
  }
}

void initFriendbox()
{
  currentDrawColorIndex = 0 + (esp_random() % (15 - 0 + 1));
  initDisplay();
  drawFriendboxLoadingScreen("Starting...", 0, "Initializing SD");
  if (initSD(false))
  {
    drawFriendboxLoadingScreen("Starting...", 250, "Initializing SD", "Done!");
  }
  else
  {
    drawFriendboxLoadingScreen("Starting...", 0, "Initializing SD", "Error! Retrying...");
    while (!SD.begin(SD_CS, sdspi))
    {
      delay(1000);
    }
    drawFriendboxLoadingScreen("Starting...", 250, "Initializing SD", "Done!");
  }
  drawFriendboxLoadingScreen("Starting...", 0, "Initializing Touch");
  if (initTouch(false))
  {
    drawFriendboxLoadingScreen("Starting...", 250, "Initializing Touch", "Done!");
  }
  drawFriendboxLoadingScreen("Starting...", 0, "Initializing Wi-Fi", NETWORK_SSID);
  if (initNetwork(NETWORK_SSID, NETWORK_PASS, LOCAL_HOSTNAME))
  {
    drawFriendboxLoadingScreen("Starting...", 250, "Initializing Wi-Fi", "Done!");
  }
  drawFriendboxLoadingScreen("Starting...", 0, "Initializing NVS");
  if (initNVS())
  {
    drawFriendboxLoadingScreen("Starting...", 500, "Initializing NVS", "Done!");
  }
  nvs.begin("Friendbox", true);
  loadImageFromSD(nvs.getUInt("lastActiveSlot", 8));
  nvs.end();
  changeScreenContext(SCREEN_CANVAS);
}

bool initNetwork(const char *netSSID, const char *netPassword, const char *hostname)
{
  WiFi.setHostname(hostname);
  WiFi.begin(netSSID, netPassword);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
  }
  // What if the network cannot ever connect? How do we handle?
  return true;
}

bool initSD(bool forceFormat)
{
#ifdef FRIENDBOX_DEBUG_MODE
  Serial.println("INFO: Initializing SD...");
#endif
  sdspi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdspi.setFrequency(40000000); // 40 MHz, explicitly otherwise will take 80mhz speed of display bus and cause corruption (?)
  if (!SD.begin(SD_CS, sdspi))
  {
#ifdef FRIENDBOX_DEBUG_MODE
    Serial.println("ERROR: SD mount failed! Is it connected properly?");
#endif
    return false;
  }
  else
  {
    return true;
#ifdef FRIENDBOX_DEBUG_MODE
    Serial.println("INFO: SD ready!");
#endif
  }
}

bool initDisplay()
{
#ifdef FRIENDBOX_DEBUG_MODE
  Serial.println("INFO: Initializing LGFX...");
#endif
  tft.init();
  tft.fillScreen(TFT_DARKCYAN);
  tft.setRotation(3); // This option enables suffering. Don't forget to account for coordinate translation!
  tft.setBrightness(255);
  tft.setColorDepth(16);

  // Allocate framebuffer in ROTATED dimensions
  canvas_framebuffer = (uint8_t *)malloc((tft.width() * tft.height()) / 2); // 76.8 KB
  if (!canvas_framebuffer)
  {
    Serial.println("FATAL: Framebuffer allocation failed!");
    while (1)
      ;
  }
  memset(canvas_framebuffer, 0, (tft.width() * tft.height()) / 2);
  return true;
}

bool initTouch(bool forceCalibrate)
{
#ifdef FRIENDBOX_DEBUG_MODE
  Serial.println("INFO: Initializing LGFX touch...");
#endif
  uint16_t calibration_data[8];
  bool calibration_data_ok = false;

  File touch_calibration_file = SD.open("/friendbox/touch_calibration_file.bin", FILE_READ);
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
      SD.remove("/friendbox/touch_calibration_file.bin");
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
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(160, 40);
    tft.setTextSize(3);
    tft.drawCenterString("Touch Needs Calibration", 240, 70);
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
    File touch_calibration_file = SD.open("/friendbox/touch_calibration_file.bin", FILE_WRITE);
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
  return true;
}

bool initNVS()
{
  nvs.begin("Friendbox", true);
  // loadImageFromSD(nvs.getUInt("lastActiveSlot", 8));
  nvs.end();
  return true;
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

void drawDitherToFB(int x, int y, int radius, uint8_t colorIndex)
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

        // Only draw even pixels.
        if ((px % 2 == 0) && py % 2 == 0)
        {
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
      drawPixelToFB(x, y, currentDrawColorIndex);
    }
  }
  // updateDisplayWithFB();
  drawFramebuffer();
}

void saveImageToSD(int slot)
{
  drawFriendboxLoadingScreen("Saving...", 0);
  if ((slot + 1) > SLOT_DROPDOWN_BUTTON_COUNT || slot < 0)
  {
    tft.print("That's not a valid save slot.");
    return;
  }
  char filename[50];
  snprintf(filename, sizeof(filename), "/sketches/slots/slot%d.fbox", slot);
  File f = SD.open(filename, FILE_WRITE);
  if (f)
  {
    f.write(canvas_framebuffer, (tft.width() * tft.height()) / 2);
    f.close();
    currentSaveSlot = slot;
    nvs.begin("Friendbox", false);
    nvs.putUInt("lastActiveSlot", currentSaveSlot);
    nvs.end();
    drawFriendboxLoadingScreen("Saved!", 250);
    drawFramebuffer();
#ifdef FRIENDBOX_DEBUG_MODE
    Serial.print("Saved image to save slot ");
    Serial.print(slot);
    Serial.println("!");
#endif
  }
  else
  {
    f.close();
    drawFriendboxLoadingScreen("ERROR: SAVE FAILED!", 1000);
    drawFramebuffer();
  }
}

void loadSketchFromSD(const char *path)
{
  drawFriendboxLoadingScreen("Loading...", 0);
  char filename[50];
  snprintf(filename, sizeof(filename), "/sketches/saved/%s", path);
  Serial.println("Loading: ");
  Serial.println(filename);
  File f = SD.open(filename, FILE_READ);
  if (f)
  {
    f.read(canvas_framebuffer, (TFT_VER_RES * TFT_HOR_RES) / 2);
    f.close();
    drawFramebuffer();
  }
  else
  {
    drawFriendboxLoadingScreen("Loading...", 500, "File Doesn't Exist :(");
  }
}

void loadImageFromSD(int slot)
{
  drawFriendboxLoadingScreen("Loading...", 0);
  if ((slot + 1) > SLOT_DROPDOWN_BUTTON_COUNT || slot < 0)
  {
    tft.print(slot);
    tft.println(" is not a valid save slot.");
    return;
  }
  char filename[50];
  snprintf(filename, sizeof(filename), "/sketches/slots/slot%d.fbox", slot);

  File f = SD.open(filename, FILE_READ);
  if (f)
  {
    f.read(canvas_framebuffer, (tft.width() * tft.height()) / 2);
    f.close();
    drawFramebuffer();
    currentSaveSlot = slot;
    nvs.begin("Friendbox", false);
    nvs.putUInt("lastActiveSlot", currentSaveSlot);
    nvs.end();
#ifdef FRIENDBOX_DEBUG_MODE
    Serial.print("Loaded image from save slot ");
    Serial.print(slot);
    Serial.println("!");
#endif
  }
  else
  {
    drawFriendboxLoadingScreen("No Sketch Saved!", 500);
    drawFramebuffer();
#ifdef FRIENDBOX_DEBUG_MODE
    Serial.print("Cant load slot ");
    Serial.print(slot);
    Serial.println(" as it does not exist.");
#endif
  }
}

// Replace specified areas with corresponding contents of the framebuffer. Passing no parameters, this will be the entire screen.
void drawFramebuffer(int x, int y, int w, int h)
{
  int x1 = max(0, x);
  int y1 = max(0, y);
  int x2 = min((int)tft.width(), x + w);
  int y2 = min((int)tft.height(), y + h);

  int width = x2 - x1;
  int height = y2 - y1;

  if (width <= 0 || height <= 0)
    return;

  static uint16_t lineBuffer[TFT_HOR_RES];

  tft.startWrite();
  tft.setAddrWindow(x1, y1, width, height);

  for (int py = y1; py < y2; py++)
  {
    for (int px = x1; px < x2; px++)
    {
      int pixelIndex = py * tft.width() + px;
      int byteIndex = pixelIndex >> 1;
      uint8_t byte = canvas_framebuffer[byteIndex];
      uint8_t colorIndex;

      if (pixelIndex & 1)
      {
        colorIndex = byte & 0x0F;
      }
      else
      {
        colorIndex = (byte >> 4) & 0x0F;
      }

      lineBuffer[px - x1] = draw_color_palette[colorIndex];
    }

    // Try the version with swap parameter
    tft.writePixelsDMA(lineBuffer, width, true); // false = don't swap bytes
  }

  tft.endWrite();
}

void networkSendFramebuffer(int userID)
{
  JsonDocument jsonDoc;
  jsonDoc["userID"] = userID;
  jsonDoc["data"] = "FASTAPI SUCKS!";

  String jsonString;
  serializeJson(jsonDoc, jsonString);

  http.begin("http://192.168.1.8:8000/posttest");
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(jsonString);

  if (httpCode == 200)
  {
    String payload = http.getString();

    Serial.println(payload);
  }
  else
  {
    Serial.printf("Error: %d\n", httpCode);
    Serial.printf("Error Payload: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
}

bool networkSendCanvas()
{
  HTTPClient http;

  // Calculate size
  size_t framebufferSize = (tft.width() * tft.height()) / 2; // 76,800 bytes

  http.begin("http://192.168.1.8:8000/sketches/upload");
  http.addHeader("Content-Type", "application/octet-stream");

  // Send raw framebuffer data
  int httpCode = http.POST(canvas_framebuffer, framebufferSize);

  if (httpCode == 200)
  {
    String response = http.getString();
    Serial.println("Sketch uploaded successfully!");
    Serial.println(response);
    http.end();
    return true;
  }
  else
  {
    Serial.printf("Upload failed: %d\n", httpCode);
    http.end();
    return false;
  }
}

void networkReceiveFramebuffer()
{
  // To implement
}

std::vector<std::string> sdGetFboxFiles()
{
  std::vector<std::string> fileNames;
  File root = SD.open("/sketches/saved");
  if (root)
  {
    File entry;
    while (entry = root.openNextFile())
    {
      if (!entry.isDirectory())
      {
        Serial.println("Found file: " + String(entry.name()));
        fileNames.push_back(entry.name());
      }
      entry.close();
    }
    root.close();
  }
  return fileNames;
}

std::vector<std::string> networkGetFriends()
{
  std::vector<std::string> friendNames;
  http.begin("http://192.168.1.8:8000/get/friends");
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.GET();

  if (httpCode == 200)
  {
    // Store payload.
    String payload = http.getString();
    http.end();
#ifdef FRIENDBOX_DEBUG_MODE
    Serial.println("Successfully retrieved friends list!");
    Serial.print("Payload:");
    Serial.println(payload);
#endif

    // Init json object.
    JsonDocument jsonDoc;
    // Deserialize JSON payload into document.
    DeserializationError error = deserializeJson(jsonDoc, payload);
    if (error)
    {
      Serial.print("Failed to parse JSON: ");
      // throw exception?
      Serial.println(error.c_str());
      return friendNames;
    }

    // Check if it's an array
    if (jsonDoc.is<JsonArray>())
    {
      JsonArray array = jsonDoc.as<JsonArray>();

      // Iterate through array and add each name
      for (JsonVariant v : array)
      {
        if (v.is<const char *>())
        {
          friendNames.push_back(v.as<const char *>());
        }
      }

      Serial.printf("Parsed %d friends\n", friendNames.size());
    }
    else
    {
      Serial.println("Response is not a JSON array!");
      return friendNames; // Return empty array.
      // throw exception?
    }
  }
  else
  {
    Serial.printf("Error: %d\n", httpCode);
    Serial.printf("Error Payload: %s\n", http.errorToString(httpCode).c_str());
    return friendNames; // Return empty array.
  }
  return friendNames;
}

void setup()
{
  // cawkins was here
  Serial.begin(115200);
#ifdef FRIENDBOX_DEBUG_MODE
  Serial.print("FriendBox ");
  Serial.print(FRIENDBOX_SOFTWARE_VERSION);
  Serial.println(" - DEBUG");
#endif
  initFriendbox();
  pinMode(HALL_SENSOR_PIN, INPUT_PULLUP);
}

void loop()
{
  handleTouch();
  handleCanvasDraw();
  handleTouchUIUpdate();
  // We just gotta run this on loop until we can set up interrupts.
  handleMenuButton(false);
}
