#include "ui_screen_canvas_menu.hpp"
#include "display.hpp"
#include "canvas.hpp"
#include "io.hpp"

static UIButton SCREEN_CANVAS_MENU_ACTION_BUTTON[SCREEN_CANVAS_UI_ACTION_BUTTON_COUNT];
static const char *SCREEN_CANVAS_MENU_ACTION_BUTTON_LABEL[SCREEN_CANVAS_UI_ACTION_BUTTON_COUNT] = {"Menu", "Tools", "Save", "Load"};
static UIButton SCREEN_CANVAS_MENU_COLOR_BUTTON[SCREEN_CANVAS_UI_COLOR_BUTTON_COUNT];
static UIButton SCREEN_CANVAS_MENU_TOOL_BUTTON[TOOL_DROPDOWN_BUTTON_COUNT];
static const char *SCREEN_CANVAS_MENU_TOOL_BUTTON_LABEL[TOOL_DROPDOWN_BUTTON_COUNT] = {"Pencil", "Brush", "Fill", "Rainbow", "Dither", "Pattern"};
static UIButton SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON[SCREEN_CANVAS_MENU_TOOL_SETTINGS_BUTTON_COUNT];
static UIButton SCREEN_CANVAS_MENU_MENU_BUTTON[MENU_DROPDOWN_BUTTON_COUNT];
static const char *SCREEN_CANVAS_MENU_MENU_BUTTON_LABEL[MENU_DROPDOWN_BUTTON_COUNT] = {"Home", "Send", "Restart", "Files", "Network"};
static UIButton SCREEN_CANVAS_MENU_SAVE_BUTTON[SLOT_DROPDOWN_BUTTON_COUNT];
static const char *SCREEN_CANVAS_MENU_SAVE_BUTTON_LABEL[SLOT_DROPDOWN_BUTTON_COUNT] = {"Slot 1", "Slot 2", "Slot 3", "Slot 4", "Slot 5", "Slot 6", "Slot 7"};
static UIButton SCREEN_CANVAS_MENU_LOAD_BUTTON[SLOT_DROPDOWN_BUTTON_COUNT];
static const char *SCREEN_CANVAS_MENU_LOAD_BUTTON_LABEL[SLOT_DROPDOWN_BUTTON_COUNT] = {"Slot 1", "Slot 2", "Slot 3", "Slot 4", "Slot 5", "Slot 6", "Slot 7"};

static std::string getUIToolName(draw_tool_id_t tool)
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

static std::string getUISubcontextName(dropdown_id_t subcontext)
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

void initUIForScreenCanvasMenu()
{
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
}

void handleTouchUIUpdateScreenCanvasMenu()
{
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
                //saveImageToSD(b);
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
                //loadImageFromSD(b);
                changeScreenContext(SCREEN_CANVAS_MENU);
            }
        }
        break;
    }
}

void drawScreenCanvasMenu()
{
    Serial.println("Drawing SCREEN_CANVAS_MENU");
    SCREEN_CANVAS_MENU_ACTION_BUTTON[1].button.setLabelText(getUIToolName(currentTool).c_str());
    if (SCREEN_CANVAS_MENU_ACTION_BUTTON[0].fillColor != draw_color_palette[currentDrawColorIndex] || !SCREEN_CANVAS_MENU_ACTION_BUTTON[0].isDrawn)
    {
        for (int col = 0; col < SCREEN_CANVAS_UI_ACTION_BUTTON_COUNT; col++)
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
