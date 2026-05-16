#include "ui_core.hpp"
#include "ui_screen_canvas_menu.hpp"
#include "ui_screen_send.hpp"
#include "ui_screen_file_browser.hpp"
#include "display.hpp"
#include "canvas.hpp"
#include "io.hpp"
#include "network.hpp"

std::vector<UIButton *> uiButtons;
UIButton *lastPressedButton;
screen_id_t currentScreen = SCREEN_STARTUP;
screen_id_t lastScreen;

static bool s_initialized[10] = {};

bool handleUIButtonPress(UIButton *targetButton, ui_button_mode_id_t buttonMode)
{
    bool isTouching = touchZ && targetButton->button.contains(touchX, touchY);
    targetButton->button.press(isTouching);

    bool justReleased = targetButton->button.justReleased();
    bool justPressed  = targetButton->button.justPressed();

    if (justReleased) targetButton->button.drawButton(false);
    if (justPressed)
    {
        targetButton->button.drawButton(true);
        lastPressedButton = targetButton;
    }

    switch (buttonMode)
    {
    case ACT_ON_PRESS:           return justPressed;
    case ACT_ON_RELEASE:         return justReleased;
    case ACT_ON_HOVER_AND_RELEASE: return justReleased && !touchZ;
    default:                     return false;
    }
}

// Aim the panel at SLOT_CANVAS for both display + draw. Used by every screen
// that doesn't keep its own backing slot.
void useCanvasSlot()
{
    tft.setCanvasAddress(LT7680_SLOT_CANVAS);
    tft.setMainImageAddress(LT7680_SLOT_CANVAS);
}

static void onEnterScreenCanvas()
{
    useCanvasSlot();
}

const ScreenHandlers screens[] = {
    /* SCREEN_CANVAS             */ {"SCREEN_CANVAS",             true,  false, nullptr,                    onEnterScreenCanvas,      nullptr,                        nullptr,                              nullptr},
    /* SCREEN_CANVAS_MENU        */ {"SCREEN_CANVAS_MENU",        true,  false, initUIForScreenCanvasMenu,  onEnterScreenCanvasMenu,  drawScreenCanvasMenu,           handleTouchUIUpdateScreenCanvasMenu,  activeSubcontextScreenCanvasMenu},
    /* SCREEN_CANVAS_SIZE_SELECT */ {"SCREEN_CANVAS_SIZE_SELECT", false, false, nullptr,                    nullptr,                  nullptr,                        nullptr,                              nullptr},
    /* SCREEN_SEND               */ {"SCREEN_SEND",               true,  false, initUIForScreenSend,        onEnterScreenSend,        drawScreenSendDefault,          handleTouchUIUpdateScreenSend,        nullptr},
    /* SCREEN_FILE_BROWSER       */ {"SCREEN_FILE_BROWSER",       true,  false, initUIForScreenFileBrowser, onEnterScreenFileBrowser, drawScreenFileBrowserDefault,   handleTouchUIUpdateScreenFileBrowser, nullptr},
    /* SCREEN_SYSTEM_MESSAGE     */ {"SCREEN_SYSTEM_MESSAGE",     true,  true,  nullptr,                    nullptr,                  nullptr,                        nullptr,                              nullptr},
    /* SCREEN_RECEIVED           */ {"SCREEN_RECEIVED",           false, false, nullptr,                    nullptr,                  nullptr,                        nullptr,                              nullptr},
    /* SCREEN_WELCOME            */ {"SCREEN_WELCOME",            false, false, nullptr,                    nullptr,                  nullptr,                        nullptr,                              nullptr},
    /* SCREEN_STARTUP            */ {"SCREEN_STARTUP",            false, false, nullptr,                    nullptr,                  nullptr,                        nullptr,                              nullptr},
    /* SCREEN_NETWORK_SETTINGS   */ {"SCREEN_NETWORK_SETTINGS",   false, false, nullptr,                    nullptr,                  nullptr,                        nullptr,                              nullptr},
};

void handleTouchUIUpdate()
{
    if (screens[currentScreen].handleTouch)
        screens[currentScreen].handleTouch();
}

std::string getUIContextName(screen_id_t screenContext)
{
    return screens[screenContext].name;
}

static bool isCanvasFamily(screen_id_t s)
{
    return s == SCREEN_CANVAS || s == SCREEN_CANVAS_MENU;
}

void changeScreenContext(screen_id_t targetScreen)
{
    const ScreenHandlers &h = screens[targetScreen];
    Serial.print("Switching Context: ");
    Serial.print(screens[currentScreen].name);
    Serial.print(" --> ");
    Serial.println(h.name);

    if (!h.implemented)
    {
#ifdef FRIENDBOX_DEBUG_MODE
        Serial.println("CRITICAL: Invalid context. Are states correct?");
#endif
        return;
    }

    bool preserveUI = h.preservePriorUI
                   || (isCanvasFamily(currentScreen) && isCanvasFamily(targetScreen))
                   || currentScreen == targetScreen;
    cleanupUIOutOfContext(!preserveUI);

    currentScreen = targetScreen;
    initUIForScreen(targetScreen);
    if (h.onEnter) h.onEnter();
    if (h.draw)    h.draw();
}

void drawFriendboxLoadingScreen(const char *subtitle, int holdTimeMs, const char *subsubtitle, const char *subsubsubtitle)
{
    lastScreen = currentScreen;
    if (currentScreen != SCREEN_SYSTEM_MESSAGE) changeScreenContext(SCREEN_SYSTEM_MESSAGE);
    tft.fillScreen(draw_color_palette[currentDrawColorIndex]);
    tft.setTextColor(draw_color_palette_text_color[currentDrawColorIndex], draw_color_palette[currentDrawColorIndex]);
    tft.setTextSize(5);
    tft.drawCenterString("FriendBox", 240, 120);
    tft.setTextSize(3);
    tft.drawCenterString(subtitle, 240, 180);
    if (subsubtitle[0] != '\0')
    {
        tft.setTextSize(3);
        tft.drawCenterString(subsubtitle, 240, 240);
    }
    if (subsubsubtitle[0] != '\0')
    {
        tft.setTextSize(2);
        tft.setTextWrap(true);
        tft.drawCenterString(subsubsubtitle, 240, 270);
    }
    delay(holdTimeMs);
    if (currentScreen != SCREEN_SYSTEM_MESSAGE) changeScreenContext(lastScreen);
}

void initUIForScreen(screen_id_t targetScreen)
{
    if (!screens[targetScreen].init) return;
    if (s_initialized[targetScreen])
    {
        Serial.print("UI already initialized for ");
        Serial.print(screens[targetScreen].name);
        Serial.println(", skipping initialization.");
        return;
    }
    Serial.print("UI not initialized for ");
    Serial.print(screens[targetScreen].name);
    Serial.println(", initializing...");
    screens[targetScreen].init();
    s_initialized[targetScreen] = true;
}

void cleanupUIOutOfContext(bool removeFromContext)
{
    int activeSub = screens[currentScreen].activeSubcontext
                        ? screens[currentScreen].activeSubcontext()
                        : 0;
    for (int i = uiButtons.size() - 1; i >= 0; i--)
    {
        if (currentScreen != uiButtons[i]->screenContext ||
            (uiButtons[i]->subcontext != 0 && uiButtons[i]->subcontext != activeSub))
        {
            if (uiButtons[i]->isDrawn)
                uiButtons[i]->isDrawn = false;

            if (removeFromContext)
            {
                s_initialized[uiButtons[i]->screenContext] = false;
                uiButtons[i]->isDrawn = false;
                uiButtons.erase(uiButtons.begin() + i);
            }
        }
    }
}

bool drawSketchPreview(const char *filepath, int x, int y, int scaleDown, bool drawBorder)
{
    FboxSourceSD src(filepath);
    if (!src.ok()) return false;

    FboxHeader hdr;
    if (!fboxReadHeader(src, hdr)) return false;

    int w = hdr.width  / scaleDown;
    int h = hdr.height / scaleDown;

    // v3: skip frame size table sequentially, then read frame 0's type byte
    uint32_t table_bytes = (uint32_t)hdr.frame_count * 4;
    uint8_t  skip[256];
    while (table_bytes > 0) {
        size_t take = table_bytes > sizeof(skip) ? sizeof(skip) : table_bytes;
        int r = src.read(skip, take);
        if (r <= 0) return false;
        table_bytes -= (uint32_t)r;
    }

    uint8_t frame_type;
    if (hdr.frame_count > 0 &&
        src.read(&frame_type, 1) == 1 && frame_type == FBOX_FRAME_I) {
        FboxRleReader rle;
        rle.begin(&src, hdr.width * hdr.height);
        bool more = true;
        uint16_t rowBuf[TFT_HOR_RES];
        int dstY = 0;
        for (int srcY = 0; srcY < hdr.height && more; srcY++) {
            for (int srcX = 0; srcX < hdr.width; srcX++) {
                int idx = rle.next();
                if (idx < 0) { more = false; break; }
                rowBuf[srcX] = draw_color_palette[idx];
            }
            if (more && srcY % scaleDown == 0) {
                tft.startWrite();
                for (int srcX = 0; srcX < hdr.width; srcX += scaleDown)
                    tft.drawPixel(x + srcX / scaleDown, y + dstY, rowBuf[srcX]);
                tft.endWrite();
                dstY++;
            }
        }
    }

    if (drawBorder)
        tft.drawRect(x - 1, y - 1, w + 2, h + 2, TFT_WHITE);

    return true;
}

void animateUIElement(UIButton *elements[], ui_anim_mode_id_t animation, int timeInMS)
{
    switch (animation)
    {
        // to be implemented
    }
}
