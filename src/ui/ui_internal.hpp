// Private surface shared between ui.cpp / ui_widgets.cpp / ui_anim.cpp.
// Not part of the public API in ui.hpp.
#pragma once
#include "ui.hpp"

namespace ui {

// Retained widget store (defined in ui.cpp). Lives in PSRAM, not BSS: internal
// SRAM is at a premium (the fbox decoder needs a contiguous 115 KB internal
// frame buffer), and the UI store is not hot enough to justify internal RAM.
constexpr int MAX_WIDGETS = 96;
extern Widget *g_widgets;     // PSRAM, MAX_WIDGETS entries (null until first use)
extern int     g_count;

// Background palette index for full-screen widget screens.
constexpr int BG_IDX = 0;   // black
constexpr int FG_IDX = 2;   // white (label text)

uint16_t  bgColor();
bool      screenIsOverlay();   // current screen draws to SLOT_UI (over canvas)
uint32_t  shownSlot();         // SDRAM slot currently driving the display
bool      fillsBg();           // does compose paint a solid background first

// Slot routing modes (Screen.slotMode).
enum SlotMode { SLOT_DIRECT = 0, SLOT_OVERLAY_CLEAN = 1, SLOT_OVERLAY_SNAPSHOT = 2 };

// Drawing (ui_widgets.cpp). widgetDraw paints one widget in its resolved rect;
// pressed=true draws the inverted/active style.
void widgetDraw(const Widget &w, bool pressed);
// List hit-test: returns the absolute item index under (tx,ty), or -1.
int  widgetListIndexAt(const Widget &w, int tx, int ty);

// Paint every visible widget into the current canvas slot (no background).
void drawWidgets();
// drawWidgets() preceded by an optional background fill (per fillsBg()).
void composeScreen();

// Animation (ui_anim.cpp).
void animPlayEnter(AnimKind k);    // reveal the freshly composed screen
void animTick();                   // advance continuous tweens (phase: stub)
bool animActive();

} // namespace ui
