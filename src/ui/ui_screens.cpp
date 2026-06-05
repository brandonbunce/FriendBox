// FriendBox screen definitions, authored on the ui:: builder API. Replaces the
// legacy per-screen init/onEnter/draw/handleTouch quadruplets and the giant
// switch(col) touch blocks: each screen is just a build() that emits widgets.
#include "ui.hpp"
#include "ui_internal.hpp"
#include "canvas.hpp"
#include "network.hpp"
#include "io.hpp"
#include "idf_compat.hpp"

#include <stdio.h>
#include <vector>
#include <string>

using namespace ui;

// ===========================================================================
// SCREEN_SEND — address book. Pick a friend to send the current canvas to.
// ===========================================================================
static std::vector<std::string> s_friends;
static int s_sendPage = 0;
static const int SEND_ROWS = 4;

static void sendToCanvasMenu(Widget &) { changeScreenContext(SCREEN_CANVAS_MENU); }
static void sendRefresh(Widget &)      { s_friends = networkGetFriends(); redraw(); }
static void sendPrev(Widget &)         { if (s_sendPage > 0) { s_sendPage--; redraw(); } }
static void sendNext(Widget &)
{
    if ((s_sendPage + 1) * SEND_ROWS < (int)s_friends.size()) { s_sendPage++; redraw(); }
}
static void sendPickFriend(int idx)
{
    drawFriendboxLoadingScreen("Sending...", 0);
    if (networkSendCanvas()) drawFriendboxLoadingScreen("Sent!", 600);
    else                     drawFriendboxLoadingScreen("Failed to send.", 1000);
    networkSendFramebuffer(idx);
    changeScreenContext(SCREEN_SEND);
}

static void buildSend()
{
    s_friends = networkGetFriends();
    beginScreen();
    beginColumn(16, 12);
        label("Send to");
        list(s_friends, &s_sendPage, SEND_ROWS, sendPickFriend).sfx(SFX_CONFIRM);
        beginRow(10);
            button("Canvas", sendToCanvasMenu).sfx(SFX_CLOSE).size(150, 48);
            button("Refresh", sendRefresh).sfx(SFX_TAP).size(150, 48);
        endRow();
        beginRow(10);
            button("Prev", sendPrev).sfx(SFX_TAP).size(150, 48);
            button("Next", sendNext).sfx(SFX_TAP).size(150, 48);
        endRow();
    endColumn();
    endScreen();
}

// ===========================================================================
// SCREEN_FILE_BROWSER — list .fbox files on the SD card; tap loads one.
// ===========================================================================
static std::vector<std::string> s_files;
static int s_filePage = 0;
static const int FILE_ROWS = 5;

static void fileBack(Widget &) { changeScreenContext(SCREEN_CANVAS_MENU); }
static void filePrev(Widget &) { if (s_filePage > 0) { s_filePage--; redraw(); } }
static void fileNext(Widget &)
{
    if ((s_filePage + 1) * FILE_ROWS < (int)s_files.size()) { s_filePage++; redraw(); }
}
static void filePick(int idx)
{
    if (idx < 0 || idx >= (int)s_files.size()) return;
    char fullPath[160];
    snprintf(fullPath, sizeof(fullPath), "/sketches/saved/%s", s_files[idx].c_str());
    changeScreenContext(SCREEN_CANVAS);
    loadSketchFromSD(fullPath);
}

static void buildFileBrowser()
{
    s_files = sdGetFboxFiles();
    beginScreen();
    beginColumn(16, 12);
        label("Files");
        list(s_files, &s_filePage, FILE_ROWS, filePick).sfx(SFX_OPEN);
        beginRow(10);
            button("Back", fileBack).sfx(SFX_CLOSE).size(140, 48);
            button("Prev", filePrev).sfx(SFX_TAP).size(140, 48);
            button("Next", fileNext).sfx(SFX_TAP).size(140, 48);
        endRow();
    endColumn();
    endScreen();
}

// ===========================================================================
// SCREEN_CANVAS_MENU — overlay hub: navigation, color picker, tool selector.
// ===========================================================================
static void menuClose(Widget &) { changeScreenContext(SCREEN_CANVAS); }
static void menuSend(Widget &)  { changeScreenContext(SCREEN_SEND); }
static void menuFiles(Widget &) { changeScreenContext(SCREEN_FILE_BROWSER); }

static void menuPickColor(Widget &w) { setDrawColor((uint8_t)payloadOf(w)); redraw(); }
static void menuPickTool(Widget &w)  { currentTool = (draw_tool_id_t)payloadOf(w); }
static void menuBrushDown(Widget &)  { setBrushSize(currentBrushRadius - 2); }
static void menuBrushUp(Widget &)    { setBrushSize(currentBrushRadius + 2); }

static const char *TOOL_LABELS[6] = { "Pencil", "Brush", "Fill", "Rain", "Dither", "Sticker" };

static void buildCanvasMenu()
{
    beginScreen();
    beginColumn(14, 12);
        beginRow(10);
            button("Close", menuClose).sfx(SFX_CLOSE).size(140, 48);
            button("Send",  menuSend).sfx(SFX_OPEN).size(140, 48);
            button("Files", menuFiles).sfx(SFX_OPEN).size(140, 48);
        endRow();

        label("Color");
        beginGrid(8, 6);
            for (int i = 0; i < 16; i++)
                button("", menuPickColor).color(i).value(i).size(0, 40).sfx(SFX_TAP);
        endGrid();

        label("Tool");
        beginGrid(3, 8);
            for (int t = 0; t < 6; t++)
                button(TOOL_LABELS[t], menuPickTool).value(t).size(0, 44).sfx(SFX_TAP);
        endGrid();

        beginRow(10);
            button("Brush -", menuBrushDown).sfx(SFX_TAP).size(150, 48);
            button("Brush +", menuBrushUp).sfx(SFX_TAP).size(150, 48);
        endRow();
    endColumn();
    endScreen();
}

// ===========================================================================
// Registration.
// ===========================================================================
void registerFriendboxScreens()
{
    // SCREEN_STARTUP — placeholder; never built.
    registerScreen(SCREEN_STARTUP,
        Screen{ "SCREEN_STARTUP", nullptr, nullptr, true, SLOT_DIRECT, ANIM_NONE });

    // SCREEN_CANVAS — freeform paint. No widgets; paint runs via customTick and
    // the sketch in SLOT_CANVAS must never be cleared on entry (SLOT_DIRECT).
    registerScreen(SCREEN_CANVAS,
        Screen{ "SCREEN_CANVAS", nullptr, handleCanvasDraw, true, SLOT_DIRECT, ANIM_NONE });

    // Menus are clean overlays on SLOT_UI: the sketch in SLOT_CANVAS survives
    // untouched, so closing a menu restores it with a single address flip.
    registerScreen(SCREEN_CANVAS_MENU,
        Screen{ "SCREEN_CANVAS_MENU", buildCanvasMenu, nullptr, false,
                SLOT_OVERLAY_CLEAN, ANIM_SLIDE_FROM_BOTTOM });
    registerScreen(SCREEN_SEND,
        Screen{ "SCREEN_SEND", buildSend, nullptr, false,
                SLOT_OVERLAY_CLEAN, ANIM_SLIDE_FROM_RIGHT });
    registerScreen(SCREEN_FILE_BROWSER,
        Screen{ "SCREEN_FILE_BROWSER", buildFileBrowser, nullptr, false,
                SLOT_OVERLAY_CLEAN, ANIM_SLIDE_FROM_RIGHT });

    // SCREEN_SYSTEM_MESSAGE — drawFriendboxLoadingScreen paints it directly.
    registerScreen(SCREEN_SYSTEM_MESSAGE,
        Screen{ "SCREEN_SYSTEM_MESSAGE", nullptr, nullptr, true, SLOT_DIRECT, ANIM_NONE });
}
