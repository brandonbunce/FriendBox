// FriendBox lightweight UI framework.
//
// Replaces the legacy UIButton/ScreenHandlers monolith. A screen is authored
// with a handful of imperative builder calls (ui::button, ui::slider, ...) that
// run once on entry and populate a retained widget store; the framework then
// owns layout, theming, drawing, touch dispatch, dirty-redraw, animation and
// SFX. No per-screen pixel math, no switch(col) touch blocks.
//
//   void buildSend() {
//       ui::beginScreen();
//       ui::beginColumn(12, 10);
//         ui::label("Send to");
//         ui::list(friends, &g_page, 5, onPick);
//         ui::beginRow(8);
//           ui::button("Canvas", onCanvas).sfx(ui::SFX_TAP);
//           ui::button("Refresh", onRefresh);
//         ui::endRow();
//       ui::endColumn();
//       ui::endScreen();
//   }
//
// The C-style screen names below (screen_id_t / currentScreen /
// changeScreenContext / drawFriendboxLoadingScreen) are kept so non-UI modules
// (canvas.cpp, io.cpp) only swap their include from "ui_core.hpp" to "ui.hpp".
#ifndef UI_HPP
#define UI_HPP

#include <stdint.h>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Screen identity (kept at global scope; legacy name compatibility).
// ---------------------------------------------------------------------------
typedef enum
{
    SCREEN_STARTUP,        // pre-boot placeholder (no widgets)
    SCREEN_CANVAS,         // freeform paint surface (custom tick, no widgets)
    SCREEN_CANVAS_MENU,    // overlay menu over the canvas
    SCREEN_SEND,           // address book
    SCREEN_FILE_BROWSER,   // SD .fbox browser
    SCREEN_SYSTEM_MESSAGE, // transient loading/error overlay
    SCREEN_COUNT
} screen_id_t;

extern screen_id_t currentScreen;
extern screen_id_t lastScreen;

void changeScreenContext(screen_id_t target);
/** Register all FriendBox screens with the framework. Call once at boot before
 *  the first changeScreenContext(). Defined in ui_screens.cpp. */
void registerFriendboxScreens();
/** Switch to the system-message overlay, paint a centered message, optionally
 *  hold, then return to the previous screen. Salvaged from the legacy core. */
void drawFriendboxLoadingScreen(const char *subtitle, int holdTimeMs = 0,
                                const char *subsubtitle = "",
                                const char *subsubsubtitle = "");

namespace ui {

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------
enum SfxId  { SFX_NONE = 0, SFX_TAP, SFX_CONFIRM, SFX_ERROR, SFX_OPEN, SFX_CLOSE, SFX_COUNT };
enum ActMode { ACT_ON_PRESS, ACT_ON_RELEASE, ACT_ON_HOVER_AND_RELEASE };
enum AnimKind
{
    ANIM_NONE = 0,
    ANIM_FADE_IN,
    ANIM_SLIDE_FROM_TOP,
    ANIM_SLIDE_FROM_BOTTOM,
    ANIM_SLIDE_FROM_LEFT,
    ANIM_SLIDE_FROM_RIGHT,
    ANIM_BOUNCE,
};
enum WidgetType { W_NONE = 0, W_LABEL, W_BUTTON, W_SLIDER, W_CHECKBOX, W_LIST };

struct Widget;
typedef void (*Callback)(Widget &);       // button / slider / checkbox events
typedef void (*ListCallback)(int index);  // list row tap (absolute item index)

// ---------------------------------------------------------------------------
// Retained widget record. POD tagged-union style (no vtables) so the whole
// store lives in a fixed BSS array with no per-frame heap churn.
// ---------------------------------------------------------------------------
struct Widget
{
    uint8_t type;       // WidgetType
    int16_t x, y, w, h; // resolved rect (filled by the layout engine)

    bool visible;
    bool dirty;         // needs a redraw this tick
    bool pressed;       // current touch-down latch
    bool enabled;

    uint8_t actMode;    // ActMode
    uint8_t sfx;        // SfxId fired on activation
    uint8_t anim;       // AnimKind enter animation
    int8_t  colorIdx;   // palette index override; -1 = theme accent
    int16_t subcontext; // 0 = always visible; else gated by screen subcontext
    int16_t payload;    // arbitrary per-widget int (see Ref::value)

    const char *label;

    Callback cb;        // button / slider / checkbox callback
    ListCallback listCb;// list row callback

    // slider / checkbox binding
    int  *ivalue;
    bool *bvalue;
    int16_t vmin, vmax;

    // list
    const std::vector<std::string> *items;
    int *page;
    int16_t rows;       // visible rows per page
};

// ---------------------------------------------------------------------------
// Ref: lightweight handle returned by builders for chained modifiers.
// ---------------------------------------------------------------------------
struct Ref
{
    int idx;
    Widget *operator->() const;
    Widget &operator*() const;
    Ref &sfx(SfxId s);
    Ref &color(int paletteIdx);
    Ref &size(int w, int h);
    Ref &sub(int subctx);
    Ref &enabled(bool e);
    Ref &anim(AnimKind a);
    Ref &mode(ActMode m);
    // Attach an integer payload to a button (read back via Widget::payload in
    // the callback). Lets one callback serve a grid of color/tool buttons.
    Ref &value(int v);
};

/** Read a button's attached integer payload inside its callback. */
int payloadOf(const Widget &w);

// ---------------------------------------------------------------------------
// Builder API. Call between beginScreen()/endScreen() inside a screen build fn.
// ---------------------------------------------------------------------------
void beginScreen();
void endScreen();

void beginColumn(int pad = 8, int gap = 8);
void endColumn();
void beginRow(int gap = 8);
void endRow();
void beginGrid(int cols, int gap = 8);
void endGrid();
void spacer(int px);

Ref label(const char *text);
Ref button(const char *text, Callback onTap);
Ref slider(int *value, int vmin, int vmax, Callback onChange = nullptr);
Ref checkbox(const char *text, bool *value, Callback onChange = nullptr);
Ref list(const std::vector<std::string> &items, int *page, int rows, ListCallback onPick);

// Default widget sizing (used when a builder is not given an explicit .size()).
constexpr int DEFAULT_W      = 200;
constexpr int BUTTON_H       = 48;
constexpr int LABEL_H        = 32;
constexpr int SLIDER_H       = 44;
constexpr int CHECKBOX_H     = 44;
constexpr int LIST_ROW_H     = 52;

// ---------------------------------------------------------------------------
// Screen dispatch.
// ---------------------------------------------------------------------------
typedef void (*BuildFn)();
typedef void (*TickFn)();   // optional per-frame hook for widgetless screens

struct Screen
{
    const char *name;
    BuildFn build;          // emits widgets (may be null for custom screens)
    TickFn  customTick;     // e.g. canvas paint; null to skip
    bool    preservePriorUI;// overlay-style: don't wipe the prior screen's pixels
    uint8_t slotMode;       // 0 = canvas slot, 1 = UI overlay slot (SLOT_UI)
    uint8_t enterAnim;      // AnimKind played when entering this screen
};

void registerScreen(screen_id_t id, const Screen &s);
void changeScreen(screen_id_t id);          // alias of changeScreenContext
screen_id_t current();
int activeSubcontext();                     // current screen's open subcontext
void setSubcontext(int subctx);             // open/close a subcontext + redraw

// ---------------------------------------------------------------------------
// Frame tick — call once per frame from the UI task (after handleTouch()).
// Runs the screen's custom tick, dispatches touch to widgets, advances
// animations, and redraws only dirty widget rects.
// ---------------------------------------------------------------------------
void tick();
void markAllDirty();
void redraw();                              // force a full widget repaint

// ---------------------------------------------------------------------------
// Theme / accent (single source of truth, seeded from currentDrawColorIndex).
// ---------------------------------------------------------------------------
void     setAccent(int paletteIdx);
int      accentIndex();
uint16_t accentColor();      // RGB565 fill
uint16_t onAccentColor();    // RGB565 readable text/outline on accent

// Render a UCG glyph (see GlyphCode in lt_assets.hpp) centered in a box at
// (x,y,w,h), recolored to fg565, composited over the current canvas slot.
// `glyphCode` is the raw GlyphCode value (kept as int to avoid the UI layer
// depending on lt_assets.hpp). 16x32 glyph; `enlarge` 1..4.
void glyphCentered(int glyphCode, int x, int y, int w, int h,
                   uint16_t fg565, uint8_t enlarge = 1);
uint16_t paletteColor(int idx);
uint16_t paletteTextColor(int idx);

// ---------------------------------------------------------------------------
// SFX. Procedurally synthesized blips pushed through a lazy, persistent UI
// I2S session. fbox playback owns I2S exclusively, so it must suspend the UI
// session around playback (one call each, see io.cpp).
// ---------------------------------------------------------------------------
void initSfx();             // synthesize clips (call once at boot)
void playSfx(SfxId id);
void suspendSfxSession();   // release I2S to fbox playback
void resumeSfxSession();    // reclaim I2S (lazy: next playSfx restarts)

// ---------------------------------------------------------------------------
// Salvaged helpers.
// ---------------------------------------------------------------------------
void useCanvasSlot();
bool sketchPreview(const char *filepath, int x, int y, int scaleDown,
                   bool drawBorder = true);

} // namespace ui

#endif // UI_HPP
