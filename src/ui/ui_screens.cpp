// FriendBox screen definitions, authored on the ui:: builder API. Replaces the
// legacy per-screen init/onEnter/draw/handleTouch quadruplets and the giant
// switch(col) touch blocks: each screen is just a build() that emits widgets.
#include "ui.hpp"
#include "ui_internal.hpp"
#include "auth.hpp"
#include "canvas.hpp"
#include "display.hpp"
#include "network.hpp"
#include "io.hpp"
#include "audio_i2s.hpp"
#include "idf_compat.hpp"

#include <esp_system.h>   // esp_restart

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <vector>
#include <string>

using namespace ui;

// ===========================================================================
// SCREEN_SEND — address book. Pick a friend to send the current canvas to.
// ===========================================================================
static std::vector<Friend>      s_friends;
static std::vector<std::string> s_friendNames;  // ui::list needs string rows
static int s_sendPage = 0;
static const int SEND_ROWS = 4;

static void sendFetchFriends()
{
    s_friends = networkGetFriends();
    s_friendNames.clear();
    for (const Friend &f : s_friends) {
        char row[64];
        if (f.streak > 0) snprintf(row, sizeof(row), "%s  (%d)", f.username.c_str(), f.streak);
        else              snprintf(row, sizeof(row), "%s", f.username.c_str());
        s_friendNames.emplace_back(row);
    }
}

static void sendToCanvasMenu(Widget &) { changeScreenContext(SCREEN_CANVAS_MENU); }
static void sendRefresh(Widget &)      { sendFetchFriends(); redraw(); }
static void sendPrev(Widget &)         { if (s_sendPage > 0) { s_sendPage--; redraw(); } }
static void sendNext(Widget &)
{
    if ((s_sendPage + 1) * SEND_ROWS < (int)s_friends.size()) { s_sendPage++; redraw(); }
}
static void sendPickFriend(int idx)
{
    if (idx < 0 || idx >= (int)s_friends.size()) return;
    // The picked friend's uuid/conv_id will address targeted sends once the
    // server grows a per-friend delivery endpoint; today the canvas goes up
    // as a regular sketch post.
    printf("sendPickFriend: %s (uuid=%s)\n",
           s_friends[idx].username.c_str(), s_friends[idx].uuid.c_str());
    drawFriendboxLoadingScreen("Sending...", 0);
    if (networkSendCanvas()) drawFriendboxLoadingScreen("Sent!", 600);
    else                     drawFriendboxLoadingScreen("Failed to send.", 1000);
    changeScreenContext(SCREEN_SEND);
}

static void buildSend()
{
    sendFetchFriends();
    beginScreen();
    beginColumn(16, 12);
        label("Send to");
        list(s_friendNames, &s_sendPage, SEND_ROWS, sendPickFriend).sfx(SFX_CONFIRM);
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

static int s_brushSize = 5;   // bound to the size sub-menu slider

static void menuPickColor(Widget &w) { setDrawColor((uint8_t)payloadOf(w)); redraw(); }
// Selecting a tool opens its size sub-menu (subcontext 1). The base palette /
// tool grid (ungated, subcontext 0) stays visible; the size slider lives in the
// reserved bottom rows and only appears while subcontext 1 is open.
static void menuPickTool(Widget &w)
{
    currentTool = (draw_tool_id_t)payloadOf(w);
    s_brushSize = currentBrushRadius;
    setSubcontext(1);
}
static void menuSizeChange(Widget &) { setBrushSize(s_brushSize); }
static void menuSizeDone(Widget &)   { setSubcontext(0); }

static const char *TOOL_LABELS[6] = { "Pencil", "Brush", "Fill", "Rain", "Dither", "Sticker" };

static void buildCanvasMenu()
{
    s_brushSize = currentBrushRadius;
    beginScreen();
    beginColumn(12, 8);
        beginRow(8);
            button("Close", menuClose).sfx(SFX_CLOSE).size(140, 48);
            button("Send",  menuSend).sfx(SFX_OPEN).size(140, 48);
            button("Files", menuFiles).sfx(SFX_OPEN).size(140, 48);
        endRow();

        label("Color");
        beginGrid(8, 6);
            for (int i = 0; i < 16; i++)
                button("", menuPickColor).color(i).value(i).size(0, 36).sfx(SFX_TAP);
        endGrid();

        label("Tool");
        beginGrid(3, 8);
            for (int t = 0; t < 6; t++)
                button(TOOL_LABELS[t], menuPickTool).value(t).size(0, 40).sfx(SFX_TAP);
        endGrid();

        // Size sub-menu — gated to subcontext 1 (shown after picking a tool).
        slider(&s_brushSize, 1, 60, menuSizeChange).sub(1).size(0, 44);
        button("Done", menuSizeDone).sub(1).sfx(SFX_CLOSE).size(0, 40);
    endColumn();
    endScreen();
}

// ===========================================================================
// SCREEN_HOME — default shell. Three big accent buttons (Draw / Sketches /
// System) over a background that silently cycles every sketch on the card
// (saved + received) by first frame, crossfading between them. The background
// lives in SLOT_CANVAS (this is a SLOT_DIRECT screen); SLOT_ANIM / SLOT_ANIM_B
// hold the two frames being faded.
// ===========================================================================
static std::vector<std::string> s_homeFiles;
static int      s_homeIdx        = 0;
static uint32_t s_homeLastSwitch = 0;
static bool     s_homeInited     = false;
static uint32_t s_homeBgSlot     = LT7680_SLOT_ANIM;  // slot holding the live bg
static const uint32_t HOME_HOLD_MS = 10000;

static void homeDraw(Widget &)
{
    // Drop into a clean canvas painted the current "paper" color (the readable
    // complement of the draw color), not the leftover home background.
    changeScreenContext(SCREEN_CANVAS);
    tft.fillScreen(draw_color_palette_text_color[currentDrawColorIndex]);
}
static void homeSketches(Widget &) { changeScreenContext(SCREEN_SKETCHES); }
static void homeSystem(Widget &)   { changeScreenContext(SCREEN_SYSTEM); }

// Gather every sketch on the card (the user's saved work plus anything received
// from friends) as full SD paths for the background slideshow.
static void homeCollectFiles()
{
    s_homeFiles.clear();
    for (const auto &f : sdGetFboxFiles())
        s_homeFiles.push_back(std::string("/sd/sketches/saved/") + f);
    for (const auto &f : sdGetReceivedFboxFiles())
        s_homeFiles.push_back(std::string("/sd/sketches/received/") + f);
}

// Decode frame 0 of a sketch into an arbitrary SDRAM slot by pointing the canvas
// at it for the duration of the (scanline-based) load, then restoring.
static void homeDecodeToSlot(const char *path, uint32_t slot)
{
    tft.setCanvasAddress(slot);
    loadSketchFromSD(path);
    tft.setCanvasAddress(LT7680_SLOT_CANVAS);
}

static void homeTick()
{
    if (!s_homeInited) {
        s_homeInited = true;
        homeCollectFiles();
        s_homeIdx    = 0;
        s_homeBgSlot = LT7680_SLOT_ANIM;
        if (s_homeFiles.empty()) {
            tft.setCanvasAddress(LT7680_SLOT_CANVAS);
            tft.fillScreen(accentColor());
        } else {
            homeDecodeToSlot(s_homeFiles[0].c_str(), s_homeBgSlot);
            tft.blitFrames(s_homeBgSlot, 0, 0, LT7680_SLOT_CANVAS, 0, 0,
                           TFT_HOR_RES, TFT_VER_RES);
        }
        redraw();                     // buttons on top of the background
        s_homeLastSwitch = millis();
        return;
    }

    if (s_homeFiles.size() < 2) return;                 // nothing to cycle
    if (millis() - s_homeLastSwitch < HOME_HOLD_MS) return;

    int next = (s_homeIdx + 1) % (int)s_homeFiles.size();
    uint32_t nextSlot = (s_homeBgSlot == LT7680_SLOT_ANIM) ? LT7680_SLOT_ANIM_B
                                                           : LT7680_SLOT_ANIM;
    homeDecodeToSlot(s_homeFiles[next].c_str(), nextSlot);

    // Crossfade nextSlot over the current bg into SLOT_CANVAS, redrawing the
    // buttons each step so they ride on top of the dissolving background.
    const int STEPS = 16;
    for (int s = 1; s <= STEPS; s++) {
        uint8_t a = (uint8_t)((31 * s) / STEPS);
        tft.blitFramesAlpha(nextSlot, 0, 0, s_homeBgSlot, 0, 0,
                            LT7680_SLOT_CANVAS, 0, 0,
                            TFT_HOR_RES, TFT_VER_RES, a);
        redraw();
        delay(40);
    }
    tft.blitFrames(nextSlot, 0, 0, LT7680_SLOT_CANVAS, 0, 0,
                   TFT_HOR_RES, TFT_VER_RES);
    redraw();

    s_homeBgSlot     = nextSlot;
    s_homeIdx        = next;
    s_homeLastSwitch = millis();
}

static void buildHome()
{
    s_homeInited = false;             // re-scan files + repaint bg on each entry
    // Center the three buttons vertically: 3×84 + 2×18 = 288 tall, so a 56px
    // spacer under the 40px top pad lands the block in the middle of 480.
    beginScreen();
    beginColumn(40, 18);
        spacer(56);
        button("Draw",     homeDraw).sfx(SFX_OPEN).size(0, 84);
        button("Sketches", homeSketches).sfx(SFX_OPEN).size(0, 84);
        button("System",   homeSystem).sfx(SFX_OPEN).size(0, 84);
    endColumn();
    endScreen();
}

// ===========================================================================
// SCREEN_SKETCHES — browse sketches received from friends; tap to play.
// ===========================================================================
static std::vector<std::string> s_rxFiles;
static int s_rxPage = 0;
static const int RX_ROWS = 5;

static void sketchesHome(Widget &) { changeScreenContext(SCREEN_HOME); }
static void sketchesPrev(Widget &) { if (s_rxPage > 0) { s_rxPage--; redraw(); } }
static void sketchesNext(Widget &)
{
    if ((s_rxPage + 1) * RX_ROWS < (int)s_rxFiles.size()) { s_rxPage++; redraw(); }
}
static void sketchesPick(int idx)
{
    if (idx < 0 || idx >= (int)s_rxFiles.size()) return;
    char full[160];
    snprintf(full, sizeof(full), "/sd/sketches/received/%s", s_rxFiles[idx].c_str());
    playFboxAnimationFromSD(full);            // blocking; touch stops playback
    changeScreenContext(SCREEN_SKETCHES);     // rebuild the list afterward
}

static void buildSketches()
{
    s_rxFiles = sdGetReceivedFboxFiles();
    beginScreen();
    beginColumn(16, 12);
        label("Sketches");
        list(s_rxFiles, &s_rxPage, RX_ROWS, sketchesPick).sfx(SFX_OPEN);
        beginRow(10);
            button("Home", sketchesHome).sfx(SFX_CLOSE).size(140, 48);
            button("Prev", sketchesPrev).sfx(SFX_TAP).size(140, 48);
            button("Next", sketchesNext).sfx(SFX_TAP).size(140, 48);
        endRow();
    endColumn();
    endScreen();
}

// ===========================================================================
// SCREEN_SYSTEM — settings. Brightness/volume sliders apply live and are
// committed to NVS when leaving the screen; plus Wi-Fi, sign-out, reboot.
// ===========================================================================
static int s_sysBrightness = 50;
static int s_sysVolume     = 100;

static void sysCommit()
{
    commitDisplayBrightness();
    commitI2SVolume();
}
static void sysBrightnessChange(Widget &) { setDisplayBrightnessLive((uint8_t)s_sysBrightness); }
static void sysVolumeChange(Widget &)     { setI2SVolumeLive((uint8_t)s_sysVolume); }
static void sysWifi(Widget &)    { sysCommit(); changeScreenContext(SCREEN_WIFI_SCAN); }
static void sysSignOut(Widget &) { sysCommit(); authForgetToken(); changeScreenContext(SCREEN_PAIRING); }
static void sysReboot(Widget &)  { sysCommit(); esp_restart(); }
static void sysHome(Widget &)    { sysCommit(); changeScreenContext(SCREEN_HOME); }

static void buildSystem()
{
    s_sysBrightness = getDisplayBrightness();
    s_sysVolume     = getI2SVolume();
    beginScreen();
    beginColumn(16, 10);
        label("System");
        label("Brightness");
        slider(&s_sysBrightness, 5, 100, sysBrightnessChange).size(0, 44);
        label("Volume");
        slider(&s_sysVolume, 0, 100, sysVolumeChange).size(0, 44);
        beginRow(10);
            button("Wi-Fi",    sysWifi).sfx(SFX_OPEN).size(150, 48);
            button("Sign out", sysSignOut).sfx(SFX_OPEN).size(150, 48);
        endRow();
        beginRow(10);
            button("Reboot", sysReboot).sfx(SFX_CONFIRM).size(150, 48);
            button("Home",   sysHome).sfx(SFX_CLOSE).size(150, 48);
        endRow();
    endColumn();
    endScreen();
}

// ===========================================================================
// SCREEN_OOBE_WELCOME — first-run intro; routes into Wi-Fi setup.
// ===========================================================================
static void oobeStart(Widget &) { changeScreenContext(SCREEN_WIFI_SCAN); }

static void buildOobeWelcome()
{
    beginScreen();
    beginColumn(28, 16);
        spacer(110);
        label("Welcome to FriendBox");
        label("Let's get you connected.");
        spacer(40);
        button("Get started", oobeStart).sfx(SFX_CONFIRM).size(0, 72);
    endColumn();
    endScreen();
}

// ===========================================================================
// SCREEN_WIFI_SCAN — pick a network from a live scan (used by OOBE + System).
// ===========================================================================
static std::vector<std::string> s_ssids;
static int  s_ssidPage = 0;
static const int SSID_ROWS = 5;
static char s_pendingSsid[33] = {0};

static void wifiScanRescan(Widget &) { s_ssids = networkScanSSIDs(); s_ssidPage = 0; redraw(); }
static void wifiScanBack(Widget &)
{
    changeScreenContext(wifiIsConfigured() ? SCREEN_SYSTEM : SCREEN_OOBE_WELCOME);
}
// Result callback for the shared keyboard: connect with the entered password.
static void wifiKeyDone(const char *pass, bool accepted)
{
    if (!accepted) { changeScreenContext(SCREEN_WIFI_SCAN); return; }
    drawFriendboxLoadingScreen("Connecting", 0, s_pendingSsid);
    if (networkSaveAndConnect(s_pendingSsid, pass)) {
        changeScreenContext(authHasToken() ? SCREEN_HOME : SCREEN_PAIRING);
    } else {
        drawFriendboxLoadingScreen("Couldn't connect", 1200, s_pendingSsid,
                                   "Check the password");
        ui::keyboardOpen("Wi-Fi password", "", wifiKeyDone);   // retry
    }
}

static void wifiScanPick(int idx)
{
    if (idx < 0 || idx >= (int)s_ssids.size()) return;
    strlcpy(s_pendingSsid, s_ssids[idx].c_str(), sizeof(s_pendingSsid));
    ui::keyboardOpen("Wi-Fi password", "", wifiKeyDone);
}

static void buildWifiScan()
{
    s_ssids = networkScanSSIDs();      // blocks ~1-2s; prior screen stays up
    beginScreen();
    beginColumn(16, 12);
        label("Choose Wi-Fi");
        list(s_ssids, &s_ssidPage, SSID_ROWS, wifiScanPick).sfx(SFX_OPEN);
        beginRow(10);
            button("Rescan", wifiScanRescan).sfx(SFX_TAP).size(150, 48);
            button("Back",   wifiScanBack).sfx(SFX_CLOSE).size(150, 48);
        endRow();
    endColumn();
    endScreen();
}

// ===========================================================================
// SCREEN_KEYBOARD — shared on-screen text-entry keyboard. Custom-tick screen
// (like SCREEN_PAIRING): no widgets, paints its own key grid and hit-tests
// touchX/Y/Z. One key model drives both drawing and hit-testing. Callers open it
// via ui::keyboardOpen() and receive the text through a result callback — no
// screen owns this keyboard. KC_ACCEPT = "Go", KC_CANCEL = "Cancel".
// ===========================================================================
enum { KC_SHIFT = -1, KC_BACK = -2, KC_LAYER = -3, KC_SPACE = -4,
       KC_ACCEPT = -5, KC_CANCEL = -6 };

struct KbKey { int16_t x, y, w, h; char cap[8]; int code; };

static char        s_kbText[129] = {0};   // entry buffer
static int         s_kbTextLen   = 0;
static const char *s_kbTitle     = "";
static bool        s_kbMask      = false; // render entry as dots
static int         s_kbMaxLen    = 64;    // clamped to <= 128
static KeyboardDoneFn s_kbDone   = nullptr;
static bool s_kbShift     = false;
static int  s_kbLayer     = 0;        // 0 = letters, 1 = symbols 1, 2 = symbols 2
static bool s_kbDirty     = true;     // full repaint needed
static bool s_kbWasTouch  = false;

// Keyboard geometry (480px wide). Three character rows, a function row, an
// action row.
static const int KB_KH = 52, KB_GAP = 6, KB_YTOP = 158;

// Lay `str` as a centered row of single-character keys at the given row index.
// `upper` uppercases letters for the shifted letter layer.
static int kbLayCharRow(KbKey *keys, int n, const char *str, int rowIdx, bool upper)
{
    int len = (int)strlen(str);
    if (len == 0) return n;
    int kw = (TFT_HOR_RES - 11 * KB_GAP) / 10;          // 10-key reference width
    if (len > 10) kw = (TFT_HOR_RES - (len + 1) * KB_GAP) / len;
    int rowW = len * kw + (len - 1) * KB_GAP;
    int x = (TFT_HOR_RES - rowW) / 2;
    int y = KB_YTOP + rowIdx * (KB_KH + KB_GAP);
    for (int i = 0; i < len; i++) {
        KbKey &k = keys[n++];
        k.x = x; k.y = y; k.w = kw; k.h = KB_KH;
        char c = str[i];
        if (upper) c = (char)toupper((unsigned char)c);
        k.cap[0] = c; k.cap[1] = 0; k.code = (unsigned char)c;
        x += kw + KB_GAP;
    }
    return n;
}

// Bottom character row: [optional shift] chars [backspace], centered.
static int kbLayRow2(KbKey *keys, int n, const char *chars, bool includeShift,
                     bool upper)
{
    int len = (int)strlen(chars);
    int kw = (TFT_HOR_RES - 11 * KB_GAP) / 10;
    int sw = kw + kw / 2;                                // wider function keys
    int y  = KB_YTOP + 2 * (KB_KH + KB_GAP);
    int nKeys = (includeShift ? 1 : 0) + len + 1;        // (+shift) chars (+back)
    int sumW  = (includeShift ? sw : 0) + len * kw + sw;
    int rowW  = sumW + (nKeys - 1) * KB_GAP;
    int x = (TFT_HOR_RES - rowW) / 2;
    if (includeShift) {
        KbKey &sh = keys[n++];
        sh.x = x; sh.y = y; sh.w = sw; sh.h = KB_KH;
        strcpy(sh.cap, upper ? "SHFT" : "shft"); sh.code = KC_SHIFT;
        x += sw + KB_GAP;
    }
    for (int i = 0; i < len; i++) {
        KbKey &k = keys[n++];
        k.x = x; k.y = y; k.w = kw; k.h = KB_KH;
        char c = chars[i];
        if (upper) c = (char)toupper((unsigned char)c);
        k.cap[0] = c; k.cap[1] = 0; k.code = (unsigned char)c;
        x += kw + KB_GAP;
    }
    KbKey &bk = keys[n++];
    bk.x = x; bk.y = y; bk.w = sw; bk.h = KB_KH;
    strcpy(bk.cap, "del"); bk.code = KC_BACK;
    return n;
}

// Build the key set for the current layer. Layers: 0 = letters, 1 = symbols
// page 1, 2 = symbols page 2 — together covering every printable ASCII symbol a
// WPA passphrase may contain. The layer key cycles 0 → 1 → 2 → 0.
static int kbBuildKeys(KbKey *keys)
{
    int n = 0;
    if (s_kbLayer == 0) {
        n = kbLayCharRow(keys, n, "qwertyuiop", 0, s_kbShift);
        n = kbLayCharRow(keys, n, "asdfghjkl",  1, s_kbShift);
        n = kbLayRow2(keys, n, "zxcvbnm", true, s_kbShift);
    } else if (s_kbLayer == 1) {
        n = kbLayCharRow(keys, n, "1234567890", 0, false);
        n = kbLayCharRow(keys, n, "!@#$%^&*()", 1, false);
        n = kbLayRow2(keys, n, "-_=+[]{}", false, false);
    } else {
        n = kbLayCharRow(keys, n, "\\|;:'\",.<>", 0, false);
        n = kbLayCharRow(keys, n, "/?~`",         1, false);
        n = kbLayRow2(keys, n, "", false, false);        // backspace only
    }

    // Function row: [layer cycle] [space].
    {
        int y = KB_YTOP + 3 * (KB_KH + KB_GAP);
        KbKey &lay = keys[n++];
        lay.x = KB_GAP; lay.y = y; lay.w = 90; lay.h = KB_KH;
        const char *ll = (s_kbLayer == 0) ? "123" : (s_kbLayer == 1) ? "=\\<" : "abc";
        strncpy(lay.cap, ll, sizeof(lay.cap) - 1);
        lay.cap[sizeof(lay.cap) - 1] = 0; lay.code = KC_LAYER;
        KbKey &sp = keys[n++];
        sp.x = KB_GAP + 90 + KB_GAP; sp.y = y;
        sp.w = TFT_HOR_RES - (KB_GAP + 90 + KB_GAP) - KB_GAP; sp.h = KB_KH;
        strcpy(sp.cap, "space"); sp.code = KC_SPACE;
    }
    // Action row: [Cancel] [Go].
    {
        int y = KB_YTOP + 4 * (KB_KH + KB_GAP);
        int kw = (TFT_HOR_RES - 3 * KB_GAP) / 2;
        KbKey &c = keys[n++];
        c.x = KB_GAP; c.y = y; c.w = kw; c.h = KB_KH;
        strcpy(c.cap, "Cancel"); c.code = KC_CANCEL;
        KbKey &cn = keys[n++];
        cn.x = KB_GAP + kw + KB_GAP; cn.y = y; cn.w = kw; cn.h = KB_KH;
        strcpy(cn.cap, "Go"); cn.code = KC_ACCEPT;
    }
    return n;
}

static void kbDrawKey(const KbKey &k, bool pressed)
{
    bool action = (k.code == KC_ACCEPT);
    uint16_t fill = pressed ? accentColor()
                   : action ? accentColor()
                            : paletteColor(1);                 // gray
    uint16_t txt  = (action || pressed) ? onAccentColor() : paletteColor(FG_IDX);
    tft.fillRoundRect(k.x, k.y, k.w, k.h, 6, fill);
    tft.drawRoundRect(k.x, k.y, k.w, k.h, 6, paletteColor(FG_IDX));
    tft.setTextColor(txt, fill);
    tft.setTextSize(2);
    tft.drawCenterString(k.cap, k.x + k.w / 2, k.y + (k.h - 16) / 2);
}

static void kbDrawHeader()
{
    const uint16_t bg = bgColor();
    tft.fillRect(0, 0, TFT_HOR_RES, 150, bg);
    tft.setTextColor(paletteColor(FG_IDX), bg);
    tft.setTextSize(3);
    tft.drawCenterString(s_kbTitle, 240, 24);
    // Entry box; masked as dots when s_kbMask, otherwise shown in clear.
    tft.drawRect(20, 84, TFT_HOR_RES - 40, 40, paletteColor(FG_IDX));
    tft.setTextColor(paletteColor(FG_IDX), bg);
    if (s_kbMask) {
        char dots[129];
        int n = s_kbTextLen < 128 ? s_kbTextLen : 128;
        for (int i = 0; i < n; i++) dots[i] = '*';
        dots[n] = 0;
        tft.drawString(dots, 30, 96);
    } else {
        tft.drawString(s_kbText, 30, 96);
    }
}

static void keyboardTick()
{
    static KbKey keys[40];
    static int nkeys = 0;

    if (s_kbDirty) {
        tft.fillScreen(bgColor());
        kbDrawHeader();
        nkeys = kbBuildKeys(keys);
        for (int i = 0; i < nkeys; i++) kbDrawKey(keys[i], false);
        s_kbDirty = false;
    }

    bool down = touchZ != 0;
    if (down && !s_kbWasTouch) {
        s_kbWasTouch = true;
        for (int i = 0; i < nkeys; i++) {
            const KbKey &k = keys[i];
            if (touchX < k.x || touchX >= k.x + k.w ||
                touchY < k.y || touchY >= k.y + k.h) continue;
            kbDrawKey(k, true);
            int cap = s_kbMaxLen < 128 ? s_kbMaxLen : 128;
            int code = k.code;
            if (code >= 32) {                       // printable character
                if (s_kbTextLen < cap) {
                    s_kbText[s_kbTextLen++] = (char)code;
                    s_kbText[s_kbTextLen]   = 0;
                }
                playSfx(SFX_TAP);
                kbDrawKey(k, false);
                kbDrawHeader();
            } else switch (code) {
                case KC_BACK:
                    if (s_kbTextLen > 0) s_kbText[--s_kbTextLen] = 0;
                    playSfx(SFX_TAP); kbDrawKey(k, false); kbDrawHeader();
                    break;
                case KC_SHIFT:
                    s_kbShift = !s_kbShift; playSfx(SFX_TAP); s_kbDirty = true;
                    break;
                case KC_LAYER:
                    s_kbLayer = (s_kbLayer + 1) % 3; playSfx(SFX_TAP); s_kbDirty = true;
                    break;
                case KC_SPACE:
                    if (s_kbTextLen < cap) {
                        s_kbText[s_kbTextLen++] = ' '; s_kbText[s_kbTextLen] = 0;
                    }
                    playSfx(SFX_TAP); kbDrawKey(k, false); kbDrawHeader();
                    break;
                case KC_CANCEL:
                    playSfx(SFX_CLOSE);
                    if (s_kbDone) s_kbDone(s_kbText, false);
                    return;
                case KC_ACCEPT:
                    playSfx(SFX_CONFIRM);
                    if (s_kbDone) s_kbDone(s_kbText, true);
                    return;
            }
            break;                                  // one key per press
        }
    } else if (!down) {
        s_kbWasTouch = false;
    }
}

static void buildKeyboard()
{
    // Text/title/callback are set by keyboardOpen(); reset only the transient
    // input mode so a fresh open starts on the letter layer. The tick paints all.
    s_kbShift = false; s_kbLayer = 0;
    // Seed the touch latch from the current state: if the finger is still down
    // from the press that opened the keyboard, treat it as already-touching so
    // the first key isn't typed until the finger lifts.
    s_kbDirty = true;  s_kbWasTouch = (touchZ != 0);
}

void ui::keyboardOpen(const char *title, const char *initial, KeyboardDoneFn onDone,
                      bool mask, int maxLen)
{
    s_kbTitle  = title ? title : "";
    s_kbDone   = onDone;
    s_kbMask   = mask;
    s_kbMaxLen = (maxLen > 0 && maxLen < 128) ? maxLen : 128;
    strlcpy(s_kbText, initial ? initial : "", sizeof(s_kbText));
    s_kbTextLen = (int)strlen(s_kbText);
    if (s_kbTextLen > s_kbMaxLen) { s_kbTextLen = s_kbMaxLen; s_kbText[s_kbTextLen] = 0; }
    changeScreenContext(SCREEN_KEYBOARD);
}

// ===========================================================================
// SCREEN_PAIRING — device login. Shows the pairing code from auth.cpp big
// enough to read across the room; the owner enters it on the web app's
// /devices page. The pairing task (core 0) owns all HTTP — this screen only
// reads the published state and repaints when it changes.
// ===========================================================================
static AuthState s_pairShownState = AuthState::UNPAIRED;
static char      s_pairShownCode[8] = {0};
static bool      s_pairDrawn = false;
static uint32_t  s_pairLastCheck = 0;

static void pairingSkip(Widget &) { changeScreenContext(SCREEN_HOME); }

static void pairingTick()
{
    uint32_t now = millis();
    if (now - s_pairLastCheck < 250) return;
    s_pairLastCheck = now;

    AuthState   st   = authState();
    const char *code = authPairingCode();
    if (s_pairDrawn && st == s_pairShownState &&
        strcmp(code, s_pairShownCode) == 0) return;
    s_pairShownState = st;
    strlcpy(s_pairShownCode, code, sizeof(s_pairShownCode));
    s_pairDrawn = true;

    // Status area between the title and the Skip button (widgets live outside it).
    const uint16_t bg = bgColor();
    const uint16_t fg = paletteColor(FG_IDX);
    tft.fillRect(0, 60, TFT_HOR_RES, 290, bg);
    tft.setTextColor(fg, bg);
    tft.setTextSize(3);
    tft.drawCenterString("Pair your FriendBox", 240, 80);
    tft.setTextSize(2);

    switch (st) {
        case AuthState::REGISTERING:
            tft.drawCenterString("Contacting server...", 240, 200);
            break;
        case AuthState::AWAITING_CLAIM:
            tft.setTextSize(9);
            tft.drawCenterString(s_pairShownCode, 240, 160);
            tft.setTextSize(2);
            tft.drawCenterString("Enter this code under Devices at", 240, 280);
            tft.drawCenterString("friendbox.chocolatedonut.dev", 240, 305);
            break;
        case AuthState::NET_ERROR:
            tft.drawCenterString("Can't reach the server.", 240, 190);
            tft.drawCenterString("Retrying...", 240, 215);
            break;
        case AuthState::PAIRED: {
            char hi[64];
            snprintf(hi, sizeof(hi), "Hi %s!", authUsername());
            tft.setTextSize(4);
            tft.drawCenterString(hi, 240, 190);
            delay(1100);
            changeScreenContext(SCREEN_HOME);
            return;
        }
        default:
            break;
    }
}

static void buildPairing()
{
    s_pairDrawn = false;   // force the status area to repaint on entry
    if (authState() == AuthState::UNPAIRED) authStartPairing();
    beginScreen();
    beginColumn(16, 12);
        spacer(350);
        button("Skip for now", pairingSkip).sfx(SFX_CLOSE).size(240, 48);
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

    // SCREEN_PAIRING — device login. Clean overlay (the sketch in SLOT_CANVAS
    // survives a Skip); the pairing code itself is painted by the custom tick.
    registerScreen(SCREEN_PAIRING,
        Screen{ "SCREEN_PAIRING", buildPairing, pairingTick, false,
                SLOT_OVERLAY_CLEAN, ANIM_FADE_IN });

    // SCREEN_HOME — default shell. SLOT_DIRECT so homeTick owns the full screen
    // (sketch background in SLOT_CANVAS with the buttons drawn on top).
    registerScreen(SCREEN_HOME,
        Screen{ "SCREEN_HOME", buildHome, homeTick, true, SLOT_DIRECT, ANIM_NONE });

    registerScreen(SCREEN_SYSTEM,
        Screen{ "SCREEN_SYSTEM", buildSystem, nullptr, false,
                SLOT_OVERLAY_CLEAN, ANIM_SLIDE_FROM_RIGHT });
    registerScreen(SCREEN_SKETCHES,
        Screen{ "SCREEN_SKETCHES", buildSketches, nullptr, false,
                SLOT_OVERLAY_CLEAN, ANIM_SLIDE_FROM_RIGHT });

    // OOBE flow.
    registerScreen(SCREEN_OOBE_WELCOME,
        Screen{ "SCREEN_OOBE_WELCOME", buildOobeWelcome, nullptr, false,
                SLOT_OVERLAY_CLEAN, ANIM_FADE_IN });
    registerScreen(SCREEN_WIFI_SCAN,
        Screen{ "SCREEN_WIFI_SCAN", buildWifiScan, nullptr, false,
                SLOT_OVERLAY_CLEAN, ANIM_SLIDE_FROM_RIGHT });
    // SCREEN_KEYBOARD — shared text entry; paints itself from keyboardTick.
    registerScreen(SCREEN_KEYBOARD,
        Screen{ "SCREEN_KEYBOARD", buildKeyboard, keyboardTick, false,
                SLOT_OVERLAY_CLEAN, ANIM_NONE });
}
