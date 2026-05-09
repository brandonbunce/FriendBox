// MUST REFACTOR THE LEGACY UI MONOLITH!!!
#ifndef UI_CORE_HPP
#define UI_CORE_HPP

#include <Arduino.h>
#include <vector>
#include <Preferences.h>
#include <LovyanGFX.h>

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

/**
 * UIButton::subcontext is a screen-private discriminator (typically a screen-local enum cast to int).
 * Convention: 0 = "always visible on this screen"; any nonzero value means the button is only
 * visible when the screen's activeSubcontext() returns the same value.
 * This lets each screen own its own subcontext type (dropdowns, sort modes, confirm states, ...)
 * without leaking the type into ui_core.
 */
struct UIButton
{
    LGFX_Button button;
    int x, y, w, h;
    bool isDrawn = false;
    screen_id_t screenContext;
    int subcontext = 0;
    int fillColor;
};

extern std::vector<UIButton *> uiButtons;
extern UIButton *lastPressedButton;

/**
 * Per-screen behavior table. One entry per screen_id_t value.
 * - implemented: false → log critical and abort the transition (matches the legacy default branch).
 * - preservePriorUI: true → cleanupUIOutOfContext(false) on entry (used by transient overlays like SCREEN_SYSTEM_MESSAGE).
 * - init/onEnter/draw/handleTouch: optional (nullptr to skip).
 *   - init runs once (dedupe-guarded by checkIfUIIsInitialized) to build LGFX_Buttons.
 *   - onEnter runs every transition into the screen, before draw — for state resets (page=0, dropdown=NONE, etc.).
 *   - draw paints the screen.
 *   - handleTouch is called every frame from handleTouchUIUpdate.
 * - activeSubcontext: optional. If set, returns the screen's currently active subcontext value
 *   (e.g. open dropdown, current sort mode). cleanupUIOutOfContext uses it to filter buttons
 *   whose `subcontext` is nonzero — only buttons matching the active subcontext stay visible.
 *   Leave nullptr if the screen has no subcontext model.
 */
struct ScreenHandlers
{
    const char *name;
    bool implemented;
    bool preservePriorUI;
    void (*init)();
    void (*onEnter)();
    void (*draw)();
    void (*handleTouch)();
    int (*activeSubcontext)();
};

extern const ScreenHandlers screens[];

// Functions
/** Check if UI is already initialized for a given screen context. If it's not, initialize it.
 * @param targetScreen The screen context we want to check for initialization and initialize if not already.
 */
void initUIForScreen(screen_id_t targetScreen);
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