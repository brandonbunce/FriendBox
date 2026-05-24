#include "ui_screen_send.hpp"
#include "display.hpp"
#include "canvas.hpp"
#include "network.hpp"
#include "idf_compat.hpp"

static UIButton SCREEN_SEND_ADDRESSBOOK_BUTTON[SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT];
static const char *SCREEN_SEND_ADDRESSBOOK_BUTTON_LABEL[SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT] = {"Friend One", "Friend Two", "Friend Three", "Friend Four", "Friend Five"};
static UIButton SCREEN_SEND_NAVI_BUTTON[SCREEN_SEND_NAVI_BUTTON_COUNT];
static const char *SCREEN_SEND_NAVI_BUTTON_LABEL[SCREEN_SEND_NAVI_BUTTON_COUNT] = {"Canvas", "Refresh", "Sort", "/\\", "\\/"};

static UIList friendListUI;

void initUIForScreenSend()
{
    for (int col = 0; col < SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT; col++)
    {
        SCREEN_SEND_ADDRESSBOOK_BUTTON[col].x = 10;
        SCREEN_SEND_ADDRESSBOOK_BUTTON[col].y = 60 * col + 15;
        SCREEN_SEND_ADDRESSBOOK_BUTTON[col].w = 300;
        SCREEN_SEND_ADDRESSBOOK_BUTTON[col].h = 50;
        SCREEN_SEND_ADDRESSBOOK_BUTTON[col].fillColor = (int)draw_color_palette[currentDrawColorIndex];
        SCREEN_SEND_ADDRESSBOOK_BUTTON[col].screenContext = SCREEN_SEND;
        SCREEN_SEND_ADDRESSBOOK_BUTTON[col].subcontext = 0;
        SCREEN_SEND_ADDRESSBOOK_BUTTON[col].button.initButtonUL(&tft, SCREEN_SEND_ADDRESSBOOK_BUTTON[col].x, SCREEN_SEND_ADDRESSBOOK_BUTTON[col].y,
                                                                SCREEN_SEND_ADDRESSBOOK_BUTTON[col].w, SCREEN_SEND_ADDRESSBOOK_BUTTON[col].h, TFT_WHITE,
                                                                SCREEN_SEND_ADDRESSBOOK_BUTTON[col].fillColor, (int)draw_color_palette_text_color[currentDrawColorIndex],
                                                                "Working...", 2, 2);
        registerUIButton(&SCREEN_SEND_ADDRESSBOOK_BUTTON[col]);
    }

    for (int col = 0; col < SCREEN_SEND_NAVI_BUTTON_COUNT; col++)
    {
        SCREEN_SEND_NAVI_BUTTON[col].x = 350;
        SCREEN_SEND_NAVI_BUTTON[col].y = 60 * col + 15;
        SCREEN_SEND_NAVI_BUTTON[col].w = 100;
        SCREEN_SEND_NAVI_BUTTON[col].h = 50;
        SCREEN_SEND_NAVI_BUTTON[col].fillColor = (int)draw_color_palette[currentDrawColorIndex];
        SCREEN_SEND_NAVI_BUTTON[col].screenContext = SCREEN_SEND;
        SCREEN_SEND_NAVI_BUTTON[col].subcontext = 0;
        SCREEN_SEND_NAVI_BUTTON[col].button.initButtonUL(&tft, SCREEN_SEND_NAVI_BUTTON[col].x, SCREEN_SEND_NAVI_BUTTON[col].y,
                                                         SCREEN_SEND_NAVI_BUTTON[col].w, SCREEN_SEND_NAVI_BUTTON[col].h, TFT_WHITE,
                                                         SCREEN_SEND_NAVI_BUTTON[col].fillColor, (int)draw_color_palette_text_color[currentDrawColorIndex],
                                                         SCREEN_SEND_NAVI_BUTTON_LABEL[col], 2, 2);
        registerUIButton(&SCREEN_SEND_NAVI_BUTTON[col]);
    }
}

void onEnterScreenSend()
{
    useCanvasSlot();
}

void drawScreenSend(int page)
{
    Serial.print("Drawing SCREEN_SEND on page ");
    Serial.println(page);
    friendListUI.listItems = networkGetFriends();
    if (!SCREEN_SEND_ADDRESSBOOK_BUTTON[0].isDrawn || friendListUI.page != page)
    {
        for (int col = 0; col < SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT; col++)
        {
            int friendIndex = col + (page * SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT);

            if (friendIndex < (int)friendListUI.listItems.size())
            {
                SCREEN_SEND_ADDRESSBOOK_BUTTON[col].fillColor = draw_color_palette[currentDrawColorIndex];
                SCREEN_SEND_ADDRESSBOOK_BUTTON[col].button.setTextColor(draw_color_palette_text_color[currentDrawColorIndex]);
                SCREEN_SEND_ADDRESSBOOK_BUTTON[col].button.setFillColor(draw_color_palette[currentDrawColorIndex]);
                SCREEN_SEND_ADDRESSBOOK_BUTTON[col].button.drawButton(false, friendListUI.listItems[friendIndex].c_str());
                SCREEN_SEND_ADDRESSBOOK_BUTTON[col].isDrawn = true;
            }
            else
            {
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
                if (page > 0)
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
            case 4: // Down
                if ((page + 1) * SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT < (int)friendListUI.listItems.size())
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

void drawScreenSendDefault() { drawScreenSend(); }

void handleTouchUIUpdateScreenSend()
{
    for (uint8_t b = 0; b < SCREEN_SEND_NAVI_BUTTON_COUNT; b++)
    {
        switch (b)
        {
        case 0: // Canvas
            if (handleUIButtonPress(&SCREEN_SEND_NAVI_BUTTON[b], ACT_ON_PRESS))
                changeScreenContext(SCREEN_CANVAS_MENU);
            break;
        case 1: // Refresh
            if (handleUIButtonPress(&SCREEN_SEND_NAVI_BUTTON[b], ACT_ON_PRESS))
                drawScreenSend(friendListUI.page);
            break;
        case 2: // Sort — not implemented yet.
            break;
        case 3: // Up
            if (friendListUI.page > 0 && handleUIButtonPress(&SCREEN_SEND_NAVI_BUTTON[b], ACT_ON_PRESS))
                drawScreenSend(friendListUI.page - 1);
            break;
        case 4: // Down
            if ((friendListUI.page + 1) * SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT < (int)friendListUI.listItems.size() &&
                handleUIButtonPress(&SCREEN_SEND_NAVI_BUTTON[b], ACT_ON_PRESS))
                drawScreenSend(friendListUI.page + 1);
            break;
        default:
            break;
        }
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
                changeScreenContext(SCREEN_SEND);
                break;
            case 1:
            case 2:
            case 3:
            case 4:
                break;
            default:
                networkSendFramebuffer(b);
                break;
            }
        }
    }
}
