#include "ui.h"
#include "display.h"
#include "canvas.h"
#include "io.h"
#include "network.h"

UIList friendListUI;
UIList fileListUI;

std::vector<UIButton *> uiButtons;
UIButton *lastPressedButton;
screen_id_t currentScreen = SCREEN_STARTUP;
screen_id_t lastScreen; // Used by drawFriendboxLoadingScreen to return to previous context after showing loading screen.
dropdown_id_t currentDropdown = DROPDOWN_NONE;

/* SCREEN_CANVAS_MENU */
UIButton SCREEN_CANVAS_MENU_ACTION_BUTTON[SCREEN_CANVAS_UI_ACTION_BUTTON_COUNT];
static const char *SCREEN_CANVAS_MENU_ACTION_BUTTON_LABEL[SCREEN_CANVAS_UI_ACTION_BUTTON_COUNT] = {"Menu", "Tools", "Save", "Load"};
UIButton SCREEN_CANVAS_MENU_COLOR_BUTTON[SCREEN_CANVAS_UI_COLOR_BUTTON_COUNT];
UIButton SCREEN_CANVAS_MENU_TOOL_BUTTON[TOOL_DROPDOWN_BUTTON_COUNT];
static const char *SCREEN_CANVAS_MENU_TOOL_BUTTON_LABEL[TOOL_DROPDOWN_BUTTON_COUNT] = {"Pencil", "Brush", "Fill", "Rainbow", "Dither", "Pattern"};
UIButton SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON_COUNT];
UIButton SCREEN_CANVAS_MENU_MENU_BUTTON[MENU_DROPDOWN_BUTTON_COUNT];
static const char *SCREEN_CANVAS_MENU_MENU_BUTTON_LABEL[MENU_DROPDOWN_BUTTON_COUNT] = {"Home", "Send", "Restart", "Files", "Network"};
UIButton SCREEN_CANVAS_MENU_SAVE_BUTTON[SLOT_DROPDOWN_BUTTON_COUNT];
static const char *SCREEN_CANVAS_MENU_SAVE_BUTTON_LABEL[SLOT_DROPDOWN_BUTTON_COUNT] = {"Slot 1", "Slot 2", "Slot 3", "Slot 4", "Slot 5", "Slot 6", "Slot 7"};
UIButton SCREEN_CANVAS_MENU_LOAD_BUTTON[SLOT_DROPDOWN_BUTTON_COUNT];
static const char *SCREEN_CANVAS_MENU_LOAD_BUTTON_LABEL[SLOT_DROPDOWN_BUTTON_COUNT] = {"Slot 1", "Slot 2", "Slot 3", "Slot 4", "Slot 5", "Slot 6", "Slot 7"};

/* SCREEN_SEND */
UIButton SCREEN_SEND_ADDRESSBOOK_BUTTON[SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT];
static const char *SCREEN_SEND_ADDRESSBOOK_BUTTON_LABEL[SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT] = {"Friend One", "Friend Two", "Friend Three", "Friend Four", "Friend Five"};
UIButton SCREEN_SEND_NAVI_BUTTON[SCREEN_SEND_NAVI_BUTTON_COUNT];
static const char *SCREEN_SEND_NAVI_BUTTON_LABEL[SCREEN_SEND_NAVI_BUTTON_COUNT] = {"Canvas", "Refresh", "Sort", "/\\", "\\/"};

/* SCREEN_FILE_BROWSER */
UIButton SCREEN_FILE_BROWSER_FILE_BUTTON[SCREEN_FILE_BROWSER_FILE_BUTTON_COUNT];
static const char *SCREEN_FILE_BROWSER_FILE_BUTTON_LABEL[SCREEN_FILE_BROWSER_FILE_BUTTON_COUNT] = {"File One", "File Two", "File Three", "File Four", "File Five"};
UIButton SCREEN_FILE_BROWSER_NAVI_BUTTON[SCREEN_FILE_BROWSER_NAVI_BUTTON_COUNT];
static const char *SCREEN_FILE_BROWSER_NAVI_BUTTON_LABEL[SCREEN_FILE_BROWSER_NAVI_BUTTON_COUNT] = {"Back", "Sort", "/\\", "\\/"};

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
        break;
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