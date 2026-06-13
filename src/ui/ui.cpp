// FriendBox UI framework core: retained widget store, deferred layout engine,
// screen dispatch, per-frame touch dispatch, and salvaged helpers.
//
// Authoring is imperative (ui::button/slider/...), but layout is *deferred*:
// builders only append widgets + record an ordered op list, so chained
// modifiers (.size(), .color(), ...) mutate the widget before endScreen()
// replays the ops through the container stack and resolves every rect.
#include "ui_internal.hpp"
#include "display.hpp"
#include "canvas.hpp"
#include "io.hpp"
#include "fbox_source.hpp"
#include "idf_compat.hpp"

#include <esp_heap_caps.h>

namespace ui {

// ---- deferred layout op list ----------------------------------------------
enum OpKind { OP_WIDGET, OP_BEGIN_COL, OP_END, OP_BEGIN_ROW, OP_BEGIN_GRID, OP_SPACER };
struct Op { uint8_t kind; int a, b, c; };   // a: widget idx / pad / cols, b: gap, c: spacer
static const int OP_CAP = MAX_WIDGETS * 2 + 16;

// ---- retained store (PSRAM, allocated on first use) -----------------------
Widget    *g_widgets = nullptr;
int        g_count   = 0;
static Op *g_ops     = nullptr;
static int g_opCount = 0;

// Allocate the widget store + op list in PSRAM. Idempotent; called before any
// builder runs. Keeps ~9 KB of internal SRAM free for the fbox decoder.
static void ensureStore()
{
    if (!g_widgets)
        g_widgets = (Widget *)heap_caps_calloc(MAX_WIDGETS, sizeof(Widget), MALLOC_CAP_SPIRAM);
    if (!g_ops)
        g_ops = (Op *)heap_caps_calloc(OP_CAP, sizeof(Op), MALLOC_CAP_SPIRAM);
}

// ---- screen registry ------------------------------------------------------
static Screen   g_screens[SCREEN_COUNT] = {};
static int      g_subcontext = 0;       // current open subcontext (0 = none)
static bool     g_overlay    = false;   // current screen draws to SLOT_UI
static bool     g_fillBg     = false;   // compose paints a solid background
static uint32_t g_shownSlot  = LT7680_SLOT_CANVAS;

// After a screen change the new screen's widgets are all freshly built with
// pressed=false. If the finger is still down from the press that triggered the
// change, a button at the same spot would read a phantom "just pressed" and
// fire instantly (e.g. System->Home landing on Home's System button). Swallow
// all widget input until the touch is physically released at least once.
static bool     g_touchGate  = false;

} // namespace ui

// Legacy-compatible globals.
screen_id_t currentScreen = SCREEN_STARTUP;
screen_id_t lastScreen    = SCREEN_STARTUP;

namespace ui {

uint16_t bgColor()        { return paletteColor(BG_IDX); }
bool     screenIsOverlay(){ return g_overlay; }
uint32_t shownSlot()      { return g_shownSlot; }
bool     fillsBg()        { return g_fillBg; }
screen_id_t current()     { return currentScreen; }
int  activeSubcontext()   { return g_subcontext; }

static bool effVisible(const Widget &w)
{
    return w.visible && (w.subcontext == 0 || w.subcontext == g_subcontext);
}

// ===========================================================================
// Builders
// ===========================================================================
static int newWidget(uint8_t type)
{
    if (g_count >= MAX_WIDGETS) return MAX_WIDGETS - 1;
    int i = g_count++;
    Widget &w = g_widgets[i];
    w = Widget{};
    w.type     = type;
    w.visible  = true;
    w.enabled  = true;
    w.colorIdx = -1;
    w.actMode  = ACT_ON_PRESS;
    w.sfx      = SFX_NONE;
    w.anim     = ANIM_NONE;
    return i;
}

static void pushOp(uint8_t kind, int a, int b = 0, int c = 0)
{
    if (!g_ops || g_opCount >= OP_CAP) return;
    g_ops[g_opCount++] = { kind, a, b, c };
}

void beginScreen() { ensureStore(); g_count = 0; g_opCount = 0; }

void beginColumn(int pad, int gap) { pushOp(OP_BEGIN_COL, pad, gap); }
void beginRow(int gap)             { pushOp(OP_BEGIN_ROW, 0, gap); }
void beginGrid(int cols, int gap)  { pushOp(OP_BEGIN_GRID, cols, gap); }
void endColumn() { pushOp(OP_END, 0); }
void endRow()    { pushOp(OP_END, 0); }
void endGrid()   { pushOp(OP_END, 0); }
void spacer(int px) { pushOp(OP_SPACER, 0, 0, px); }

Ref label(const char *text)
{
    int i = newWidget(W_LABEL);
    g_widgets[i].label = text;
    pushOp(OP_WIDGET, i);
    return Ref{ i };
}

Ref button(const char *text, Callback onTap)
{
    int i = newWidget(W_BUTTON);
    g_widgets[i].label = text;
    g_widgets[i].cb    = onTap;
    pushOp(OP_WIDGET, i);
    return Ref{ i };
}

Ref slider(int *value, int vmin, int vmax, Callback onChange)
{
    int i = newWidget(W_SLIDER);
    Widget &w = g_widgets[i];
    w.ivalue = value;
    w.vmin = vmin; w.vmax = vmax;
    w.cb = onChange;
    pushOp(OP_WIDGET, i);
    return Ref{ i };
}

Ref checkbox(const char *text, bool *value, Callback onChange)
{
    int i = newWidget(W_CHECKBOX);
    Widget &w = g_widgets[i];
    w.label = text;
    w.bvalue = value;
    w.cb = onChange;
    w.actMode = ACT_ON_RELEASE;
    pushOp(OP_WIDGET, i);
    return Ref{ i };
}

Ref list(const std::vector<std::string> &items, int *page, int rows, ListCallback onPick)
{
    int i = newWidget(W_LIST);
    Widget &w = g_widgets[i];
    w.items = &items;
    w.page  = page;
    w.rows  = rows;
    w.listCb = onPick;
    pushOp(OP_WIDGET, i);
    return Ref{ i };
}

// ===========================================================================
// Ref modifiers
// ===========================================================================
Widget *Ref::operator->() const { return &g_widgets[idx]; }
Widget &Ref::operator*()  const { return g_widgets[idx]; }
Ref &Ref::sfx(SfxId s)      { g_widgets[idx].sfx = (uint8_t)s; return *this; }
Ref &Ref::color(int p)      { g_widgets[idx].colorIdx = (int8_t)p; return *this; }
Ref &Ref::size(int w, int h){ g_widgets[idx].w = w; g_widgets[idx].h = h; return *this; }
Ref &Ref::sub(int s)        { g_widgets[idx].subcontext = (int16_t)s; return *this; }
Ref &Ref::enabled(bool e)   { g_widgets[idx].enabled = e; return *this; }
Ref &Ref::anim(AnimKind a)  { g_widgets[idx].anim = (uint8_t)a; return *this; }
Ref &Ref::mode(ActMode m)   { g_widgets[idx].actMode = (uint8_t)m; return *this; }
Ref &Ref::value(int v)      { g_widgets[idx].payload = (int16_t)v; return *this; }

int payloadOf(const Widget &w) { return w.payload; }

// ===========================================================================
// Deferred layout engine
// ===========================================================================
enum CKind { C_COL, C_ROW, C_GRID };
struct Container
{
    uint8_t kind;
    int outerX, outerY;       // top-left where this container began
    int contentX, contentY;   // inset by pad
    int availW;               // usable content width
    int cursorX, cursorY;     // running placement cursor
    int pad, gap;
    int cols, col, rowMaxH;   // grid / row tracking
    int contentRight, contentBottom;
};
static Container g_stack[10];
static int       g_depth = 0;

static int defaultHeight(const Widget &w)
{
    switch (w.type) {
        case W_BUTTON:   return BUTTON_H;
        case W_LABEL:    return LABEL_H;
        case W_SLIDER:   return SLIDER_H;
        case W_CHECKBOX: return CHECKBOX_H;
        case W_LIST:     return (w.rows > 0 ? w.rows : 1) * LIST_ROW_H;
        default:         return BUTTON_H;
    }
}

static void placeWidget(int idx)
{
    if (g_depth == 0) return;
    Container &c = g_stack[g_depth - 1];
    Widget &w = g_widgets[idx];

    if (w.h == 0) w.h = defaultHeight(w);

    if (c.kind == C_GRID) {
        int cellW = (c.availW - (c.cols - 1) * c.gap) / (c.cols > 0 ? c.cols : 1);
        w.w = cellW;
        w.x = c.contentX + c.col * (cellW + c.gap);
        w.y = c.cursorY;
        if (w.h > c.rowMaxH) c.rowMaxH = w.h;
        c.col++;
        if (c.col >= c.cols) { c.col = 0; c.cursorY += c.rowMaxH + c.gap; c.rowMaxH = 0; }
    } else if (c.kind == C_ROW) {
        if (w.w == 0) w.w = DEFAULT_W;
        w.x = c.cursorX;
        w.y = c.contentY;
        c.cursorX += w.w + c.gap;
        if (w.h > c.rowMaxH) c.rowMaxH = w.h;
    } else { // C_COL
        if (w.w == 0) w.w = c.availW;     // full content width by default
        w.x = c.contentX;
        w.y = c.cursorY;
        c.cursorY += w.h + c.gap;
    }

    int r = w.x + w.w, b = w.y + w.h;
    if (r > c.contentRight)  c.contentRight  = r;
    if (b > c.contentBottom) c.contentBottom = b;
}

static void pushContainer(uint8_t kind, int pad, int gap, int cols)
{
    Container c{};
    c.kind = kind; c.pad = pad; c.gap = gap; c.cols = cols;
    if (g_depth == 0) {
        c.outerX = 0; c.outerY = 0; c.availW = TFT_HOR_RES;
    } else {
        Container &p = g_stack[g_depth - 1];
        c.outerX = p.cursorX; c.outerY = p.cursorY;
        c.availW = p.availW - (c.outerX - p.contentX) - 2 * pad;
    }
    c.contentX = c.outerX + pad;
    c.contentY = c.outerY + pad;
    c.cursorX = c.contentX;
    c.cursorY = c.contentY;
    c.contentRight = c.contentX;
    c.contentBottom = c.contentY;
    if (c.availW < 0) c.availW = 0;
    g_stack[g_depth++] = c;
}

static void popContainer()
{
    if (g_depth == 0) return;
    Container c = g_stack[--g_depth];
    int outerW = (c.contentRight - c.outerX) + c.pad;
    int outerH = (c.contentBottom - c.outerY) + c.pad;
    if (outerW < 0) outerW = 0;
    if (outerH < 0) outerH = 0;
    if (g_depth == 0) return;
    Container &p = g_stack[g_depth - 1];
    if (p.kind == C_ROW) {
        p.cursorX += outerW + p.gap;
        if (outerH > p.rowMaxH) p.rowMaxH = outerH;
    } else { // column-like
        p.cursorY += outerH + p.gap;
    }
    int r = c.outerX + outerW, b = c.outerY + outerH;
    if (r > p.contentRight)  p.contentRight  = r;
    if (b > p.contentBottom) p.contentBottom = b;
}

void endScreen()
{
    g_depth = 0;
    pushContainer(C_COL, 0, 0, 0);   // implicit root
    for (int i = 0; i < g_opCount; i++) {
        const Op &op = g_ops[i];
        switch (op.kind) {
            case OP_WIDGET:     placeWidget(op.a); break;
            case OP_BEGIN_COL:  pushContainer(C_COL, op.a, op.b, 0); break;
            case OP_BEGIN_ROW:  pushContainer(C_ROW, 0, op.b, 0); break;
            case OP_BEGIN_GRID: pushContainer(C_GRID, 0, op.b, op.a); break;
            case OP_END:        popContainer(); break;
            case OP_SPACER: {
                if (g_depth) {
                    Container &c = g_stack[g_depth - 1];
                    if (c.kind == C_ROW) c.cursorX += op.c; else c.cursorY += op.c;
                }
                break;
            }
        }
    }
    g_depth = 0;
}

// ===========================================================================
// Compose / redraw
// ===========================================================================
void drawWidgets()
{
    for (int i = 0; i < g_count; i++) {
        if (effVisible(g_widgets[i]))
            widgetDraw(g_widgets[i], g_widgets[i].pressed);
    }
}

void composeScreen()
{
    if (g_fillBg) tft.fillScreen(bgColor());
    drawWidgets();
}

void redraw()
{
    tft.setCanvasAddress(g_shownSlot);
    composeScreen();
}

void markAllDirty()
{
    for (int i = 0; i < g_count; i++) g_widgets[i].dirty = true;
}

void setSubcontext(int subctx)
{
    if (subctx == g_subcontext) return;
    g_subcontext = subctx;
    redraw();
}

// ===========================================================================
// Screen dispatch
// ===========================================================================
void registerScreen(screen_id_t id, const Screen &s)
{
    if (id >= 0 && id < SCREEN_COUNT) g_screens[id] = s;
}

void changeScreen(screen_id_t target) { changeScreenContext(target); }

} // namespace ui

void changeScreenContext(screen_id_t target)
{
    using namespace ui;
    if (target < 0 || target >= SCREEN_COUNT) return;
    const Screen &s = g_screens[target];

    lastScreen    = currentScreen;
    currentScreen = target;
    g_subcontext  = 0;

    // Slot routing:
    //   SLOT_DIRECT           — draw straight onto SLOT_CANVAS (the sketch).
    //   SLOT_OVERLAY_CLEAN    — draw a fresh menu onto SLOT_UI; SLOT_CANVAS is
    //                           untouched, so leaving restores the sketch.
    //   SLOT_OVERLAY_SNAPSHOT — copy the sketch into SLOT_UI, then draw over it.
    g_overlay    = (s.slotMode != SLOT_DIRECT);
    g_fillBg     = (s.slotMode == SLOT_OVERLAY_CLEAN);
    g_shownSlot  = g_overlay ? LT7680_SLOT_UI : LT7680_SLOT_CANVAS;

    if (s.slotMode == SLOT_OVERLAY_SNAPSHOT) {
        tft.blitFrames(LT7680_SLOT_CANVAS, 0, 0, LT7680_SLOT_UI, 0, 0,
                       TFT_HOR_RES, TFT_VER_RES);
    }
    tft.setMainImageAddress(g_shownSlot);
    tft.setCanvasAddress(g_shownSlot);

    g_count = 0; g_opCount = 0; g_depth = 0;
    if (s.build) s.build();

    g_touchGate = true;   // ignore the lingering press until the finger lifts
    animPlayEnter((AnimKind)s.enterAnim);
}

namespace ui {

// ===========================================================================
// Per-frame touch dispatch
// ===========================================================================
static bool pointIn(const Widget &w, int px, int py)
{
    return px >= w.x && px < w.x + w.w && py >= w.y && py < w.y + w.h;
}

static void dispatchButtonLike(Widget &w)
{
    bool inside = touchZ && pointIn(w, touchX, touchY);
    bool was = w.pressed;
    w.pressed = inside;
    bool justPressed  = inside && !was;
    bool justReleased = !inside && was;

    if (justPressed)  widgetDraw(w, true);
    if (justReleased) widgetDraw(w, false);

    bool fire = false;
    switch (w.actMode) {
        case ACT_ON_PRESS:            fire = justPressed; break;
        case ACT_ON_RELEASE:          fire = justReleased; break;
        case ACT_ON_HOVER_AND_RELEASE:fire = justReleased && !touchZ; break;
    }
    if (!fire) return;

    if (w.type == W_CHECKBOX && w.bvalue) {
        *w.bvalue = !*w.bvalue;
        widgetDraw(w, false);
    }
    playSfx((SfxId)w.sfx);
    if (w.cb) w.cb(w);
}

static void dispatchSlider(Widget &w)
{
    bool inside = touchZ && pointIn(w, touchX, touchY);
    if (touchZ && (w.pressed || inside)) {
        w.pressed = true;
        int margin = w.h / 2;
        int lo = w.x + margin, hi = w.x + w.w - margin;
        int span = hi - lo;
        int v = w.vmin;
        if (span > 0) {
            int t = (int)touchX - lo;
            if (t < 0) t = 0;
            if (t > span) t = span;
            v = w.vmin + (int)((int64_t)t * (w.vmax - w.vmin) / span);
        }
        if (w.ivalue && *w.ivalue != v) {
            *w.ivalue = v;
            widgetDraw(w, false);
            if (w.cb) w.cb(w);
        }
    } else if (!touchZ) {
        w.pressed = false;
    }
}

static void dispatchList(Widget &w)
{
    bool inside = touchZ && pointIn(w, touchX, touchY);
    bool was = w.pressed;
    w.pressed = inside;
    if (inside && !was) {
        int idx = widgetListIndexAt(w, touchX, touchY);
        if (idx >= 0) {
            playSfx((SfxId)w.sfx);
            if (w.listCb) w.listCb(idx);
        }
    }
}

void tick()
{
    const Screen &s = g_screens[currentScreen];
    if (s.customTick) s.customTick();

    // Touch gate: after a screen change, suppress all widget input until the
    // finger lifts, so the press that caused the change can't fire on the new
    // screen too. Clears the frame touchZ first goes to 0.
    if (g_touchGate) {
        if (!touchZ) g_touchGate = false;
        if (animActive()) animTick();
        return;
    }

    screen_id_t before = currentScreen;
    for (int i = 0; i < g_count; i++) {
        Widget &w = g_widgets[i];
        if (!w.enabled || !effVisible(w)) continue;
        switch (w.type) {
            case W_BUTTON:
            case W_CHECKBOX: dispatchButtonLike(w); break;
            case W_SLIDER:   dispatchSlider(w);     break;
            case W_LIST:     dispatchList(w);       break;
            default: break;
        }
        // A callback may have rebuilt the store (changeScreen). Bail out.
        if (currentScreen != before) return;
    }

    if (animActive()) animTick();
}

// ===========================================================================
// Salvaged helpers
// ===========================================================================
void useCanvasSlot()
{
    tft.setCanvasAddress(LT7680_SLOT_CANVAS);
    tft.setMainImageAddress(LT7680_SLOT_CANVAS);
}

bool sketchPreview(const char *filepath, int x, int y, int scaleDown, bool drawBorder)
{
    FboxSourceSD src(filepath);
    if (!src.ok()) return false;

    FboxHeader hdr;
    if (!fboxReadHeader(src, hdr)) return false;

    int w = hdr.width  / scaleDown;
    int h = hdr.height / scaleDown;

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

} // namespace ui

// ===========================================================================
// drawFriendboxLoadingScreen — transient full-screen message overlay.
// ===========================================================================
void drawFriendboxLoadingScreen(const char *subtitle, int holdTimeMs,
                                const char *subsubtitle, const char *subsubsubtitle)
{
    lastScreen = currentScreen;
    if (currentScreen != SCREEN_SYSTEM_MESSAGE) changeScreenContext(SCREEN_SYSTEM_MESSAGE);
    tft.fillScreen(draw_color_palette[currentDrawColorIndex]);
    tft.setTextColor(draw_color_palette_text_color[currentDrawColorIndex],
                     draw_color_palette[currentDrawColorIndex]);
    tft.setTextSize(5);
    tft.drawCenterString("FriendBox", 240, 120);
    tft.setTextSize(3);
    tft.drawCenterString(subtitle, 240, 180);
    if (subsubtitle[0] != '\0') {
        tft.setTextSize(3);
        tft.drawCenterString(subsubtitle, 240, 240);
    }
    if (subsubsubtitle[0] != '\0') {
        tft.setTextSize(2);
        tft.setTextWrap(true);
        tft.drawCenterString(subsubsubtitle, 240, 270);
    }
    delay(holdTimeMs);
    if (currentScreen == SCREEN_SYSTEM_MESSAGE) changeScreenContext(lastScreen);
}
