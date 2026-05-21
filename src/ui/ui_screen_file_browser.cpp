#include "ui_screen_file_browser.hpp"
#include "display.hpp"
#include "canvas.hpp"
#include "io.hpp"
#include "idf_compat.hpp"

static UIButton SCREEN_FILE_BROWSER_FILE_BUTTON[SCREEN_FILE_BROWSER_FILE_BUTTON_COUNT];
static UIButton SCREEN_FILE_BROWSER_NAVI_BUTTON[SCREEN_FILE_BROWSER_NAVI_BUTTON_COUNT];
static const char *SCREEN_FILE_BROWSER_NAVI_BUTTON_LABEL[SCREEN_FILE_BROWSER_NAVI_BUTTON_COUNT] = {"Back", "Sort", "/\\", "\\/"};

static UIList fileListUI;

void initUIForScreenFileBrowser()
{
    for (int col = 0; col < SCREEN_FILE_BROWSER_FILE_BUTTON_COUNT; col++)
    {
        SCREEN_FILE_BROWSER_FILE_BUTTON[col].x = 10;
        SCREEN_FILE_BROWSER_FILE_BUTTON[col].y = 60 * col + 15;
        SCREEN_FILE_BROWSER_FILE_BUTTON[col].w = 300;
        SCREEN_FILE_BROWSER_FILE_BUTTON[col].h = 50;
        SCREEN_FILE_BROWSER_FILE_BUTTON[col].fillColor = (int)draw_color_palette[currentDrawColorIndex];
        SCREEN_FILE_BROWSER_FILE_BUTTON[col].screenContext = SCREEN_FILE_BROWSER;
        SCREEN_FILE_BROWSER_FILE_BUTTON[col].subcontext = 0;
        SCREEN_FILE_BROWSER_FILE_BUTTON[col].button.initButtonUL(&tft, SCREEN_FILE_BROWSER_FILE_BUTTON[col].x, SCREEN_FILE_BROWSER_FILE_BUTTON[col].y,
                                                                 SCREEN_FILE_BROWSER_FILE_BUTTON[col].w, SCREEN_FILE_BROWSER_FILE_BUTTON[col].h, TFT_WHITE,
                                                                 SCREEN_FILE_BROWSER_FILE_BUTTON[col].fillColor, (int)draw_color_palette_text_color[currentDrawColorIndex],
                                                                 "File Name", 2, 2);
        uiButtons.push_back(&SCREEN_FILE_BROWSER_FILE_BUTTON[col]);
    }

    for (int col = 0; col < SCREEN_FILE_BROWSER_NAVI_BUTTON_COUNT; col++)
    {
        SCREEN_FILE_BROWSER_NAVI_BUTTON[col].x = 350;
        SCREEN_FILE_BROWSER_NAVI_BUTTON[col].y = 60 * col + 15;
        SCREEN_FILE_BROWSER_NAVI_BUTTON[col].w = 100;
        SCREEN_FILE_BROWSER_NAVI_BUTTON[col].h = 50;
        SCREEN_FILE_BROWSER_NAVI_BUTTON[col].fillColor = (int)draw_color_palette[currentDrawColorIndex];
        SCREEN_FILE_BROWSER_NAVI_BUTTON[col].screenContext = SCREEN_FILE_BROWSER;
        SCREEN_FILE_BROWSER_NAVI_BUTTON[col].subcontext = 0;
        SCREEN_FILE_BROWSER_NAVI_BUTTON[col].button.initButtonUL(&tft, SCREEN_FILE_BROWSER_NAVI_BUTTON[col].x, SCREEN_FILE_BROWSER_NAVI_BUTTON[col].y,
                                                                 SCREEN_FILE_BROWSER_NAVI_BUTTON[col].w, SCREEN_FILE_BROWSER_NAVI_BUTTON[col].h, TFT_WHITE,
                                                                 SCREEN_FILE_BROWSER_NAVI_BUTTON[col].fillColor, (int)draw_color_palette_text_color[currentDrawColorIndex],
                                                                 SCREEN_FILE_BROWSER_NAVI_BUTTON_LABEL[col], 2, 2);
        uiButtons.push_back(&SCREEN_FILE_BROWSER_NAVI_BUTTON[col]);
    }
}

void onEnterScreenFileBrowser()
{
    useCanvasSlot();
    fileListUI.page = 0;
    fileListUI.listItems = sdGetFboxFiles();
}

void drawScreenFileBrowser(int page)
{
    Serial.print("Drawing SCREEN_FILE_BROWSER on page ");
    Serial.println(page);
    if (!SCREEN_FILE_BROWSER_FILE_BUTTON[0].isDrawn || fileListUI.page != page)
    {
        for (int col = 0; col < SCREEN_FILE_BROWSER_FILE_BUTTON_COUNT; col++)
        {
            int fileIndex = col + (page * SCREEN_FILE_BROWSER_FILE_BUTTON_COUNT);

            if (fileIndex < (int)fileListUI.listItems.size())
            {
                SCREEN_FILE_BROWSER_FILE_BUTTON[col].fillColor = draw_color_palette[currentDrawColorIndex];
                SCREEN_FILE_BROWSER_FILE_BUTTON[col].button.setTextColor(draw_color_palette_text_color[currentDrawColorIndex]);
                SCREEN_FILE_BROWSER_FILE_BUTTON[col].button.setFillColor(draw_color_palette[currentDrawColorIndex]);
                SCREEN_FILE_BROWSER_FILE_BUTTON[col].button.drawButton(false, fileListUI.listItems[fileIndex].c_str());
                SCREEN_FILE_BROWSER_FILE_BUTTON[col].isDrawn = true;
            }
            else
            {
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
                if (page > 0)
                {
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
                if ((page + 1) * SCREEN_FILE_BROWSER_FILE_BUTTON_COUNT < (int)fileListUI.listItems.size())
                {
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

void drawScreenFileBrowserDefault() { drawScreenFileBrowser(); }

void handleTouchUIUpdateScreenFileBrowser()
{
    for (uint8_t b = 0; b < SCREEN_FILE_BROWSER_NAVI_BUTTON_COUNT; b++)
    {
        switch (b)
        {
        case 0: // Back
            if (handleUIButtonPress(&SCREEN_FILE_BROWSER_NAVI_BUTTON[b], ACT_ON_PRESS))
                changeScreenContext(SCREEN_CANVAS_MENU);
            break;
        case 1: // Sort — not implemented yet.
            break;
        case 2: // Up
            if (fileListUI.page > 0 && handleUIButtonPress(&SCREEN_FILE_BROWSER_NAVI_BUTTON[b], ACT_ON_PRESS))
                drawScreenFileBrowser(fileListUI.page - 1);
            break;
        case 3: // Down
            if ((fileListUI.page + 1) * SCREEN_FILE_BROWSER_FILE_BUTTON_COUNT < (int)fileListUI.listItems.size() &&
                handleUIButtonPress(&SCREEN_FILE_BROWSER_NAVI_BUTTON[b], ACT_ON_PRESS))
                drawScreenFileBrowser(fileListUI.page + 1);
            break;
        default:
            break;
        }
    }

    for (uint8_t b = 0; b < SCREEN_FILE_BROWSER_FILE_BUTTON_COUNT; b++)
    {
        if (handleUIButtonPress(&SCREEN_FILE_BROWSER_FILE_BUTTON[b], ACT_ON_PRESS))
        {
            int fileIndex = b + (fileListUI.page * SCREEN_FILE_BROWSER_FILE_BUTTON_COUNT);

            if (fileIndex < (int)fileListUI.listItems.size())
            {
                const char *filename = fileListUI.listItems[fileIndex].c_str();
                Serial.printf("Select Filename [%d]: %s\n", fileIndex, filename);

                char fullPath[128];
                snprintf(fullPath, sizeof(fullPath), "/sketches/saved/%s", filename);
                changeScreenContext(SCREEN_CANVAS);
                loadSketchFromSD(fullPath);
            }
            else
            {
                Serial.printf("ERROR: Index %d out of bounds (size: %d)\n",
                              fileIndex, (int)fileListUI.listItems.size());
            }
        }
    }
}
