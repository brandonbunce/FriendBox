#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LovyanGFX.h>
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

struct UIButton
{
  LGFX_Button button;
  int x, y, w, h;
  bool isDrawn = false;
  screen_id_t screenContext;
  dropdown_id_t dropdownContext = DROPDOWN_NONE;
  int fillColor;
};

struct Friend {
  String name;
  int userID;
};

std::vector<UIButton *> uiButtons;

UIButton *lastPressedButton;

static draw_tool_id_t currentTool = TOOL_BRUSH;
static screen_id_t currentScreen;
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
#define CANVAS_DRAW_MENU_TOP_BAR_DIST_FROM_TOP_PX 5
#define CANVAS_DRAW_MENU_TOP_BAR_HEIGHT_PX 50
#define CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX 110
#define CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX 25
#define ACTION_BUTTON_SPACING ((TFT_HOR_RES - (ACTION_BUTTON_COUNT * CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX)) / (ACTION_BUTTON_COUNT + 1))
#define ACTION_BUTTON_X_POS(col) (ACTION_BUTTON_SPACING + ((col) * (CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX + ACTION_BUTTON_SPACING)))
#define ACTION_BUTTON_COUNT 4
UIButton SCREEN_CANVAS_MENU_ACTION_BUTTON[ACTION_BUTTON_COUNT];
static const char *SCREEN_CANVAS_MENU_ACTION_BUTTON_LABEL[ACTION_BUTTON_COUNT] = {"MENU", "TOOLS", "SAVE", "LOAD"};

#define CANVAS_DRAW_MENU_BOTTOM_BAR_DIST_FROM_BOTTOM_PX 5
#define CANVAS_DRAW_MENU_BOTTOM_BAR_HEIGHT_PX 25
#define CANVAS_DRAW_MENU_BOTTOM_BAR_ITEM_WIDTH_PX 27
#define COLOR_BUTTON_SPACING ((TFT_HOR_RES - (16 * CANVAS_DRAW_MENU_BOTTOM_BAR_ITEM_WIDTH_PX)) / 16)
#define COLOR_BUTTON_X_POS(col) (COLOR_BUTTON_SPACING + ((col) * (CANVAS_DRAW_MENU_BOTTOM_BAR_ITEM_WIDTH_PX + COLOR_BUTTON_SPACING)))
#define COLOR_BUTTON_COUNT 16 // This should technically be static at 16 due to 4-bit color.
UIButton SCREEN_CANVAS_MENU_COLOR_BUTTON[COLOR_BUTTON_COUNT];

#define CANVAS_DRAW_MENU_DROPDOWN_DIST_BETWEEN_ITEMS 5
#define TOOL_DROPDOWN_BUTTON_COUNT 6
UIButton SCREEN_CANVAS_MENU_TOOL_BUTTON[TOOL_DROPDOWN_BUTTON_COUNT];
static const char *SCREEN_CANVAS_MENU_TOOL_BUTTON_LABEL[TOOL_DROPDOWN_BUTTON_COUNT] = {"PENCIL", "BRUSH", "FILL", "RAINBOW", "DITHER", "PATTERN"};
UIButton SCREEN_CANVAS_MENU_SETTINGS_BUTTON;
static const char *SCREEN_CANVAS_MENU_SETTINGS_BUTTON_LABEL = "SET SIZE";
#define MENU_DROPDOWN_BUTTON_COUNT 3
UIButton SCREEN_CANVAS_MENU_MENU_BUTTON[MENU_DROPDOWN_BUTTON_COUNT];
static const char *SCREEN_CANVAS_MENU_MENU_BUTTON_LABEL[MENU_DROPDOWN_BUTTON_COUNT] = {"GO HOME", "SEND", "REBOOT"};
#define SLOT_DROPDOWN_BUTTON_COUNT 7
UIButton SCREEN_CANVAS_MENU_SAVE_BUTTON[SLOT_DROPDOWN_BUTTON_COUNT];
static const char *SCREEN_CANVAS_MENU_SAVE_BUTTON_LABEL[SLOT_DROPDOWN_BUTTON_COUNT] = {"SLOT 1", "SLOT 2", "SLOT 3", "SLOT 4", "SLOT 5", "SLOT 6", "SLOT 7"};
UIButton SCREEN_CANVAS_MENU_LOAD_BUTTON[SLOT_DROPDOWN_BUTTON_COUNT];
static const char *SCREEN_CANVAS_MENU_LOAD_BUTTON_LABEL[SLOT_DROPDOWN_BUTTON_COUNT] = {"SLOT 1", "SLOT 2", "SLOT 3", "SLOT 4", "SLOT 5", "SLOT 6", "SLOT 7"};

/* SCREEN_SEND */
#define SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT 5
UIButton SCREEN_SEND_ADDRESSBOOK_BUTTON[SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT];
static const char *SCREEN_SEND_ADDRESSBOOK_BUTTON_LABEL[SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT] = {"Friend One", "Friend Two", "Friend Three", "Friend Four", "Friend Five"};
#define SCREEN_SEND_NAVI_BUTTON_COUNT 5
UIButton SCREEN_SEND_NAVI_BUTTON[SCREEN_SEND_NAVI_BUTTON_COUNT];
static const char *SCREEN_SEND_NAVI_BUTTON_LABEL[SCREEN_SEND_NAVI_BUTTON_COUNT] = {"Canvas", "Refresh", "Sort", "/\\", "\\/"};

// Network
#define LOCAL_HOSTNAME "friendbox"
HTTPClient http;

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
void initScreenCanvasMenuButtons();
void drawScreenCanvasMenu();
void initScreenSendButtons();
void drawScreenSend();
void drawClearScreen();
void saveImageToSD(int slot);
void loadImageFromSD(int slot);
void drawFramebuffer(int x = 0, int y = 0, int w = TFT_HOR_RES, int h = TFT_VER_RES);
void networkSendFramebuffer(int userID);
void networkReceiveFramebuffer();
std::vector<std::string> networkGetFriends();
void cleanupUIOutOfContext(bool destroyElement = false);
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
      drawBrushToFB(touchX, touchY, 2, currentDrawColorIndex);
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
    for (uint8_t b = 0; b < ACTION_BUTTON_COUNT; b++)
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
          case 2:          // Reboot
            esp_restart(); // obviously
            break;
          }
        }
      }
      break;
    case DROPDOWN_TOOLS:
      for (uint8_t b = 0; b < TOOL_DROPDOWN_BUTTON_COUNT; b++)
      {
        if (handleUIButtonPress(&SCREEN_CANVAS_MENU_TOOL_BUTTON[b], ACT_ON_HOVER_AND_RELEASE))
        {
          currentTool = draw_tool_id_t(b);
        }
      }
      break;
    case DROPDOWN_SAVE:
      for (uint8_t b = 0; b < SLOT_DROPDOWN_BUTTON_COUNT; b++)
      {
        if (handleUIButtonPress(&SCREEN_CANVAS_MENU_SAVE_BUTTON[b], ACT_ON_HOVER_AND_RELEASE))
        {
          saveImageToSD(b);
        }
      }
      break;
    case DROPDOWN_LOAD:
      for (uint8_t b = 0; b < SLOT_DROPDOWN_BUTTON_COUNT; b++)
      {
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
      if (handleUIButtonPress(&SCREEN_SEND_NAVI_BUTTON[b], ACT_ON_PRESS))
      {
        switch (b)
        {
        case 0: // Canvas
          changeScreenContext(SCREEN_CANVAS_MENU);
          break;
        default:
          // Not implemented yet.
          break;
        }
        // Draw pressed dropdown.
        // drawScreenCanvasMenu();
      }
    }
    for (uint8_t b = 0; b < SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT; b++)
    {
      if (handleUIButtonPress(&SCREEN_SEND_ADDRESSBOOK_BUTTON[b], ACT_ON_PRESS))
      {
        switch (b)
        {
        default:
          networkSendFramebuffer(b);
          break;
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
        // drawCanvasMenu(true, false, false, false, false);
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
        // drawCanvasMenu(false, false, false, false, false);
        if (currentScreen != SCREEN_CANVAS)
          changeScreenContext(SCREEN_CANVAS);
      }
    }
  }
}

void changeScreenContext(screen_id_t targetScreen)
{
  switch (targetScreen)
  {
  case SCREEN_CANVAS:
    Serial.println("Drawing New Context: SCREEN_CANVAS");
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
    Serial.println("Drawing New Context: SCREEN_CANVAS_MENU");
    if (currentScreen != SCREEN_CANVAS)
    {
      currentScreen = SCREEN_CANVAS_MENU;
      cleanupUIOutOfContext(true);
      initScreenCanvasMenuButtons();
    }
    currentDropdown = DROPDOWN_NONE;
    currentScreen = SCREEN_CANVAS_MENU;
    drawScreenCanvasMenu();
    break;
  case SCREEN_SEND:
    Serial.println("Drawing New Context: SCREEN_SEND");
    if (currentScreen != SCREEN_SEND)
    {
      currentScreen = SCREEN_SEND;
      cleanupUIOutOfContext(true);
      initScreenSendButtons();
    }
    currentScreen = SCREEN_SEND;
    drawScreenSend();
    break;
  default:
#ifdef FRIENDBOX_DEBUG_MODE
    Serial.println("CRITICAL: Invalid context for drawing canvas menu. Are states correct?");
#endif
  }
}

/* Initialize canvas menu buttons.*/
void initScreenSendButtons()
{
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
                                                            "USELESS", 2, 2);
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
}

void drawScreenSend()
{
  Serial.print("Drawing SCREEN_SEND.");
  std::vector<std::string> friendNames = networkGetFriends();
  if (!SCREEN_SEND_ADDRESSBOOK_BUTTON[0].isDrawn)
  {
    for (int col = 0; col < SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT; col++)
    {
      SCREEN_SEND_ADDRESSBOOK_BUTTON[col].fillColor = draw_color_palette[currentDrawColorIndex];
      SCREEN_SEND_ADDRESSBOOK_BUTTON[col].button.setTextColor(draw_color_palette_text_color[currentDrawColorIndex]);
      SCREEN_SEND_ADDRESSBOOK_BUTTON[col].button.setFillColor(draw_color_palette[currentDrawColorIndex]);
      SCREEN_SEND_ADDRESSBOOK_BUTTON[col].button.drawButton(false, friendNames[col].c_str());
      SCREEN_SEND_ADDRESSBOOK_BUTTON[col].isDrawn = true;
    }
  }
  if (!SCREEN_SEND_NAVI_BUTTON[0].isDrawn)
  {
    for (int col = 0; col < SCREEN_SEND_NAVI_BUTTON_COUNT; col++)
    {
      SCREEN_SEND_NAVI_BUTTON[col].fillColor = draw_color_palette[currentDrawColorIndex];
      SCREEN_SEND_NAVI_BUTTON[col].button.setTextColor(draw_color_palette_text_color[currentDrawColorIndex]);
      SCREEN_SEND_NAVI_BUTTON[col].button.setFillColor(draw_color_palette[currentDrawColorIndex]);
      SCREEN_SEND_NAVI_BUTTON[col].button.drawButton();
      SCREEN_SEND_NAVI_BUTTON[col].isDrawn = true;
    }
  }
  friendNames.end();
}

void drawSystemMessage()
{
}

void drawScreenCanvasMenu()
{
  Serial.println("drawing canvas menu.");
  // Draw Action Bar if Color Doesn't Match or Not Drawn
  if (SCREEN_CANVAS_MENU_ACTION_BUTTON[0].fillColor != draw_color_palette[currentDrawColorIndex] || !SCREEN_CANVAS_MENU_ACTION_BUTTON[0].isDrawn)
  {
    for (int col = 0; col < ACTION_BUTTON_COUNT; col++)
    {
      SCREEN_CANVAS_MENU_ACTION_BUTTON[col].fillColor = draw_color_palette[currentDrawColorIndex];
      SCREEN_CANVAS_MENU_ACTION_BUTTON[col].button.setTextColor(draw_color_palette_text_color[currentDrawColorIndex]);
      SCREEN_CANVAS_MENU_ACTION_BUTTON[col].button.setFillColor(draw_color_palette[currentDrawColorIndex]);
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

/**
 * Search all registered UI buttons, determine if they are in context, and if not, write over them with canvas.
 * @param destroyElement Should we destroy the object instead of marking it as undrawn?
 */
void cleanupUIOutOfContext(bool destroyElement)
{
  for (int i = uiButtons.size() - 1; i >= 0; i--)
  {
    if ((currentScreen != uiButtons[i]->screenContext || (currentDropdown != uiButtons[i]->dropdownContext && uiButtons[i]->dropdownContext != DROPDOWN_NONE)) && uiButtons[i]->isDrawn)
    {
      drawFramebuffer(uiButtons[i]->x, uiButtons[i]->y, uiButtons[i]->w, uiButtons[i]->h);
      if (destroyElement)
      {
        Serial.println("Undrawing UI and Deleting from Context.");
        uiButtons[i]->isDrawn = false;
        uiButtons.erase(uiButtons.begin() + i);
      }
      else
      {
        // Serial.println("Undrawing UI.");
        uiButtons[i]->isDrawn = false;
      }
    }
  }
}

/* Initialize canvas menu buttons.*/
void initScreenCanvasMenuButtons()
{
  // Init Action Buttons (Top)
  for (int col = 0; col < ACTION_BUTTON_COUNT; col++)
  {
    SCREEN_CANVAS_MENU_ACTION_BUTTON[col].x = ACTION_BUTTON_X_POS(col);
    SCREEN_CANVAS_MENU_ACTION_BUTTON[col].y = CANVAS_DRAW_MENU_TOP_BAR_DIST_FROM_TOP_PX;
    SCREEN_CANVAS_MENU_ACTION_BUTTON[col].w = CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX;
    SCREEN_CANVAS_MENU_ACTION_BUTTON[col].h = CANVAS_DRAW_MENU_TOP_BAR_HEIGHT_PX;
    SCREEN_CANVAS_MENU_ACTION_BUTTON[col].fillColor = (int)draw_color_palette[currentDrawColorIndex];
    SCREEN_CANVAS_MENU_ACTION_BUTTON[col].screenContext = SCREEN_CANVAS_MENU;
    SCREEN_CANVAS_MENU_ACTION_BUTTON[col].dropdownContext = DROPDOWN_NONE;
    SCREEN_CANVAS_MENU_ACTION_BUTTON[col].button.initButtonUL(&tft, SCREEN_CANVAS_MENU_ACTION_BUTTON[col].x, SCREEN_CANVAS_MENU_ACTION_BUTTON[col].y, SCREEN_CANVAS_MENU_ACTION_BUTTON[col].w, SCREEN_CANVAS_MENU_ACTION_BUTTON[col].h, TFT_WHITE,
                                                              SCREEN_CANVAS_MENU_ACTION_BUTTON[col].fillColor, (int)draw_color_palette_text_color[currentDrawColorIndex],
                                                              SCREEN_CANVAS_MENU_ACTION_BUTTON_LABEL[col], 3, 3);
    // push back pointer instead of unique object
    uiButtons.push_back(&SCREEN_CANVAS_MENU_ACTION_BUTTON[col]);
  }

  // Init Color Buttons (Bottom)
  for (int col = 0; col < 16; col++)
  {
    SCREEN_CANVAS_MENU_COLOR_BUTTON[col].x = COLOR_BUTTON_X_POS(col);
    SCREEN_CANVAS_MENU_COLOR_BUTTON[col].y = (tft.height() - (CANVAS_DRAW_MENU_BOTTOM_BAR_HEIGHT_PX + CANVAS_DRAW_MENU_BOTTOM_BAR_DIST_FROM_BOTTOM_PX));
    SCREEN_CANVAS_MENU_COLOR_BUTTON[col].w = CANVAS_DRAW_MENU_BOTTOM_BAR_ITEM_WIDTH_PX;
    SCREEN_CANVAS_MENU_COLOR_BUTTON[col].h = CANVAS_DRAW_MENU_BOTTOM_BAR_HEIGHT_PX;
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
    SCREEN_CANVAS_MENU_MENU_BUTTON[col].x = ACTION_BUTTON_X_POS(0);
    SCREEN_CANVAS_MENU_MENU_BUTTON[col].y = ((col + 1) * (CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX + CANVAS_DRAW_MENU_DROPDOWN_DIST_BETWEEN_ITEMS) + CANVAS_DRAW_MENU_TOP_BAR_DIST_FROM_TOP_PX + CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX);
    SCREEN_CANVAS_MENU_MENU_BUTTON[col].w = CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX;
    SCREEN_CANVAS_MENU_MENU_BUTTON[col].h = CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX;
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
    SCREEN_CANVAS_MENU_TOOL_BUTTON[col].x = ACTION_BUTTON_X_POS(1);
    SCREEN_CANVAS_MENU_TOOL_BUTTON[col].y = ((col + 1) * (CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX + CANVAS_DRAW_MENU_DROPDOWN_DIST_BETWEEN_ITEMS) + CANVAS_DRAW_MENU_TOP_BAR_DIST_FROM_TOP_PX + CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX);
    SCREEN_CANVAS_MENU_TOOL_BUTTON[col].w = CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX;
    SCREEN_CANVAS_MENU_TOOL_BUTTON[col].h = CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX;
    SCREEN_CANVAS_MENU_TOOL_BUTTON[col].fillColor = draw_color_palette[currentDrawColorIndex];
    SCREEN_CANVAS_MENU_TOOL_BUTTON[col].screenContext = SCREEN_CANVAS_MENU;
    SCREEN_CANVAS_MENU_TOOL_BUTTON[col].dropdownContext = DROPDOWN_TOOLS;
    SCREEN_CANVAS_MENU_TOOL_BUTTON[col].button.initButtonUL(&tft, SCREEN_CANVAS_MENU_TOOL_BUTTON[col].x, SCREEN_CANVAS_MENU_TOOL_BUTTON[col].y, SCREEN_CANVAS_MENU_TOOL_BUTTON[col].w, SCREEN_CANVAS_MENU_TOOL_BUTTON[col].h,
                                                            TFT_WHITE, (int)draw_color_palette[currentDrawColorIndex], (int)draw_color_palette_text_color[currentDrawColorIndex],
                                                            SCREEN_CANVAS_MENU_TOOL_BUTTON_LABEL[col], 2, 2);
    uiButtons.push_back(&SCREEN_CANVAS_MENU_TOOL_BUTTON[col]);
  }
  // Init Save Buttons
  for (int col = 0; col < SLOT_DROPDOWN_BUTTON_COUNT; col++)
  {
    SCREEN_CANVAS_MENU_SAVE_BUTTON[col].x = ACTION_BUTTON_X_POS(2);
    SCREEN_CANVAS_MENU_SAVE_BUTTON[col].y = ((col + 1) * (CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX + CANVAS_DRAW_MENU_DROPDOWN_DIST_BETWEEN_ITEMS) + CANVAS_DRAW_MENU_TOP_BAR_DIST_FROM_TOP_PX + CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX);
    SCREEN_CANVAS_MENU_SAVE_BUTTON[col].w = CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX;
    SCREEN_CANVAS_MENU_SAVE_BUTTON[col].h = CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX;
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
    SCREEN_CANVAS_MENU_LOAD_BUTTON[col].x = ACTION_BUTTON_X_POS(3);
    SCREEN_CANVAS_MENU_LOAD_BUTTON[col].y = ((col + 1) * (CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX + CANVAS_DRAW_MENU_DROPDOWN_DIST_BETWEEN_ITEMS) + CANVAS_DRAW_MENU_TOP_BAR_DIST_FROM_TOP_PX + CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX);
    SCREEN_CANVAS_MENU_LOAD_BUTTON[col].w = CANVAS_DRAW_MENU_TOP_BAR_ITEM_WIDTH_PX;
    SCREEN_CANVAS_MENU_LOAD_BUTTON[col].h = CANVAS_DRAW_MENU_TOP_BAR_ITEM_HEIGHT_PX;
    SCREEN_CANVAS_MENU_LOAD_BUTTON[col].fillColor = draw_color_palette[currentDrawColorIndex];
    SCREEN_CANVAS_MENU_LOAD_BUTTON[col].screenContext = SCREEN_CANVAS_MENU;
    SCREEN_CANVAS_MENU_LOAD_BUTTON[col].dropdownContext = DROPDOWN_LOAD;
    SCREEN_CANVAS_MENU_LOAD_BUTTON[col].button.initButtonUL(&tft, SCREEN_CANVAS_MENU_LOAD_BUTTON[col].x, SCREEN_CANVAS_MENU_LOAD_BUTTON[col].y, SCREEN_CANVAS_MENU_LOAD_BUTTON[col].w, SCREEN_CANVAS_MENU_LOAD_BUTTON[col].h,
                                                            TFT_WHITE, (int)draw_color_palette[currentDrawColorIndex], (int)draw_color_palette_text_color[currentDrawColorIndex],
                                                            SCREEN_CANVAS_MENU_LOAD_BUTTON_LABEL[col], 2, 2);
    uiButtons.push_back(&SCREEN_CANVAS_MENU_LOAD_BUTTON[col]);
  }
}

void initFriendbox()
{
  initDisplay();
  // Initial Display Presentation
  tft.fillScreen(draw_color_palette[currentDrawColorIndex]);
  tft.setTextColor(draw_color_palette_text_color[currentDrawColorIndex], draw_color_palette[currentDrawColorIndex]);
  tft.setTextSize(4);
  tft.drawCenterString("FriendBox", 240, 20);
  tft.setTextSize(3);
  tft.drawCenterString(FRIENDBOX_SOFTWARE_VERSION, 240, 60);
  tft.setCursor(0, 160);
  tft.setTextSize(2);
  tft.print("Initializing SD...");
  if (initSD(false))
  {
    tft.println("Done!");
  }
  else
  {
    tft.print("ERROR! Retrying.");
    while (!SD.begin(SD_CS, sdspi))
    {
      tft.print(".");
      delay(1000);
    }
    tft.println("Done!");
  }
  tft.print("Initializing Touch...");
  if (initTouch(false))
  {
    tft.println("Done!");
  }
  tft.print("Initializing Network...");
  if (initNetwork(NETWORK_SSID, NETWORK_PASS, LOCAL_HOSTNAME))
  {
    tft.println("Done!");
  }
  initScreenCanvasMenuButtons();
  tft.print("Initializing NVS...");
  if (initNVS())
  {
    tft.println("Done!");
  }
  delay(500);
  nvs.begin("Friendbox", true);
  loadImageFromSD(nvs.getUInt("lastActiveSlot", 8));
  nvs.end();
}

bool initNetwork(const char *netSSID, const char *netPassword, const char *hostname)
{
  tft.println("Connecting to ");
  tft.print(netSSID);
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
  tft.println(WiFi.localIP());
  // What if the network cannot ever connect? How do we handle?
  return true;
}

bool initSD(bool forceFormat)
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
  tft.fillScreen(draw_color_palette[currentDrawColorIndex]);
  tft.setTextColor(draw_color_palette_text_color[currentDrawColorIndex], draw_color_palette[currentDrawColorIndex]);
  tft.setTextSize(4);
  tft.drawCenterString("FriendBox", 240, 120);
  tft.setTextSize(3);
  tft.drawCenterString("Saving...", 240, 180);
  if ((slot + 1) > SLOT_DROPDOWN_BUTTON_COUNT || slot < 0)
  {
    tft.print("That's not a valid save slot.");
    return;
  }
  char filename[20];
  snprintf(filename, sizeof(filename), "/slot%d.bin", slot);
  File f = SD.open(filename, FILE_WRITE);
  if (f)
  {
    f.write(canvas_framebuffer, (tft.width() * tft.height()) / 2);
    f.close();
    currentSaveSlot = slot;
    nvs.begin("Friendbox", false);
    nvs.putUInt("lastActiveSlot", currentSaveSlot);
    nvs.end();
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
    Serial.println("Failure.");
  }
}

void loadImageFromSD(int slot)
{
  tft.fillScreen(draw_color_palette[currentDrawColorIndex]);
  tft.setTextColor(draw_color_palette_text_color[currentDrawColorIndex], draw_color_palette[currentDrawColorIndex]);
  tft.setTextSize(4);
  tft.drawCenterString("FriendBox", 240, 120);
  tft.setTextSize(3);
  tft.drawCenterString("Loading...", 240, 180);
  if ((slot + 1) > SLOT_DROPDOWN_BUTTON_COUNT || slot < 0)
  {
    tft.print(slot);
    tft.println(" is not a valid save slot.");
    return;
  }
  char filename[20];
  snprintf(filename, sizeof(filename), "/slot%d.bin", slot);

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
    tft.setTextSize(2);
    tft.drawCenterString("No sketch saved to that slot!", 240, 220);
    delay(500);
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

void networkReceiveFramebuffer()
{
  // To implement
}

std::vector<std::string> networkGetFriends()
{
  std::vector<std::string> friendNames;
  http.begin("http://192.168.1.8:8000/get/friends");
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.GET();

  if (httpCode == 200)
  {
    Serial.println("Successfully retrieved friends list!");
    String payload = http.getString();
    Serial.print("Payload:");
    Serial.println(payload);

    JsonDocument jsonDoc;
    DeserializationError error = deserializeJson(jsonDoc, payload);
    if (error)
    {
      Serial.print("Failed to parse JSON: ");
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
          Serial.println(v.as<const char *>());
          friendNames.push_back(v.as<const char *>());
        }
      }

      Serial.printf("Parsed %d friends\n", friendNames.size());
    }
    else
    {
      Serial.println("Response is not a JSON array!");
    }
  }
  else
  {
    Serial.printf("Error: %d\n", httpCode);
    Serial.printf("Error Payload: %s\n", http.errorToString(httpCode).c_str());
  }
  friendNames.push_back("idgaf");
  http.end();
  return friendNames;
}

void setup()
{
  // cawkins was here
  Serial.begin(115200);
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
