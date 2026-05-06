// MUST REFACTOR THE LEGACY UI MONOLITH!!!
#ifndef UI_HPP
#define UI_HPP

#include <Arduino.h>
#include <vector>
#include <Preferences.h>
#include <LovyanGFX.h>

#define SCREEN_CANVAS_UI_ACTION_BAR_DIST_FROM_TOP_PX 5
#define SCREEN_CANVAS_UI_ACTION_BAR_HEIGHT 50
#define SCREEN_CANVAS_UI_ACTION_BAR_ITEM_WIDTH_PX 110
#define SCREEN_CANVAS_UI_ACTION_BAR_ITEM_HEIGHT_PX 25
#define SCREEN_CANVAS_UI_ACTION_BUTTON_SPACING ((TFT_HOR_RES - (SCREEN_CANVAS_UI_ACTION_BUTTON_COUNT * SCREEN_CANVAS_UI_ACTION_BAR_ITEM_WIDTH_PX)) / (SCREEN_CANVAS_UI_ACTION_BUTTON_COUNT + 1))
#define SCREEN_CANVAS_UI_ACTION_BUTTON_X_POS(col) (SCREEN_CANVAS_UI_ACTION_BUTTON_SPACING + ((col) * (SCREEN_CANVAS_UI_ACTION_BAR_ITEM_WIDTH_PX + SCREEN_CANVAS_UI_ACTION_BUTTON_SPACING)))
#define SCREEN_CANVAS_UI_ACTION_BUTTON_COUNT 4

#define SCREEN_CANVAS_UI_COLOR_BAR_DIST_FROM_BOTTOM_PX 5
#define SCREEN_CANVAS_UI_COLOR_BAR_ITEM_HEIGHT_PX 50
#define SCREEN_CANVAS_UI_COLOR_BAR_ITEM_WIDTH_PX 27
#define SCREEN_CANVAS_UI_COLOR_BUTTON_SPACING ((TFT_HOR_RES - (16 * SCREEN_CANVAS_UI_COLOR_BAR_ITEM_WIDTH_PX)) / 16)
#define SCREEN_CANVAS_UI_COLOR_BUTTON_X_POS(col) (SCREEN_CANVAS_UI_COLOR_BUTTON_SPACING + ((col) * (SCREEN_CANVAS_UI_COLOR_BAR_ITEM_WIDTH_PX + SCREEN_CANVAS_UI_COLOR_BUTTON_SPACING)))
#define SCREEN_CANVAS_UI_COLOR_BUTTON_COUNT 16 // This should technically be static at 16 due to 4-bit color.

#define CANVAS_DRAW_MENU_DROPDOWN_DIST_BETWEEN_ITEMS 5
#define TOOL_DROPDOWN_BUTTON_COUNT 6
#define SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON_COUNT 6
#define MENU_DROPDOWN_BUTTON_COUNT 5
#define SLOT_DROPDOWN_BUTTON_COUNT 7

/* SCREEN_SEND */
#define SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT 5
#define SCREEN_SEND_NAVI_BUTTON_COUNT 5

/* SCREEN_FILE_BROWSER */
#define SCREEN_FILE_BROWSER_FILE_BUTTON_COUNT 5
#define SCREEN_FILE_BROWSER_NAVI_BUTTON_COUNT 4

struct UIList
{
    std::vector<std::string> listItems;
    int page;
};

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

extern screen_id_t currentScreen;
extern screen_id_t lastScreen; // Used by drawFriendboxLoadingScreen to return to previous context after showing loading screen.
extern dropdown_id_t currentDropdown;

struct UIButton
{
    LGFX_Button button;
    int x, y, w, h;
    bool isDrawn = false;
    screen_id_t screenContext;
    dropdown_id_t dropdownContext = DROPDOWN_NONE;
    int fillColor;
};

// Functions
/** Check if UI is already initialized for a given screen context. If it's not, initialize it.
 * @param targetScreen The screen context we want to check for initialization and initialize if not already.
 */
void initUIForScreen(screen_id_t targetScreen);
void drawScreenCanvasMenu();
/**
 * Draw
 * @param page Starting from zero, show which "page" of friends we're showing in the address book. Each page shows 5 friends, so page 0 shows friends 0-4, page 1 shows friends 5-9, etc.
 */
void drawScreenSend(int page = 0);
void drawScreenFileBrowser(int page = 0);
bool drawSketchPreview(const char *filepath, int x, int y, int scaleDown, bool drawBorder = true);
/** Switch context to loading screen and show while waiting for operations or network activity.
 * @param subtitle Subtitle to show under loading text, can be used to give more context on what we're waiting for.
 * @param holdTimeMs How long should we hold before returning?
 * @param subsubtitle Self-explanatory.
 * @param subsubsubtitle Self-explanatory.
 */
void drawFriendboxLoadingScreen(const char *subtitle, int holdTimeMs = 0, const char *subsubtitle = "", const char *subsubsubtitle = "");
/**
 * Search all registered UI buttons, determine if they are in context, and if not, write over them with canvas.
 * @param removeFromContext On top of redrawing over element, should we also remove it from the UI elements vector?
 */
void cleanupUIOutOfContext(bool destroyElement = false);
/** Loop through current UI elements to see if any exist belonging to the target context.
 * @param targetScreen Which screen context are we checking for?
 * @return bool True if we find an element in the target context, false if we loop through all elements without finding one.
 */
bool checkIfUIIsInitialized(screen_id_t targetScreen);
void changeScreenContext(screen_id_t targetScreen);
/** Run this to check the target button for inputs, and register a logical press when the button is pressed according to mode.
 * @param targetButton The button we are checking for input on.
 * @param buttonMode The mode we are checking for input in, this determines when we register a logical press.
 */
bool handleUIButtonPress(UIButton *targetButton, ui_button_mode_id_t buttonMode = ACT_ON_PRESS);
void handleTouchUIUpdate();

#endif