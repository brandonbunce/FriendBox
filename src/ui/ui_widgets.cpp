// Per-widget rendering + hit-testing. Everything is a GPU kick on the LT7680
// (rounded-rect / rect / circle via the Geometric Drawing Engine) plus a
// LovyanGFX text pass — no per-pixel software fills.
#include "ui_internal.hpp"
#include "display.hpp"
#include "canvas.hpp"

namespace ui {

static inline uint16_t fillColorOf(const Widget &w)
{
    return (w.colorIdx >= 0) ? paletteColor(w.colorIdx) : accentColor();
}
static inline uint16_t textColorOf(const Widget &w)
{
    return (w.colorIdx >= 0) ? paletteTextColor(w.colorIdx) : onAccentColor();
}

// Center a text label inside a rect (datum-independent: drawCenterString
// centers horizontally about cx; we offset y to vertically center for size).
static void centerLabel(const char *txt, int cx, int cy, int textSize, uint16_t color)
{
    if (!txt || !txt[0]) return;
    tft.setTextSize(textSize);
    tft.setTextColor(color);
    tft.drawCenterString(txt, cx, cy - (8 * textSize));
}

static void drawButton(const Widget &w, bool pressed)
{
    uint16_t accent = fillColorOf(w);
    uint16_t on     = textColorOf(w);
    uint16_t fill   = pressed ? on : accent;
    uint16_t txt    = pressed ? accent : on;
    int r = (w.w < w.h ? w.w : w.h) / 4;

    tft.startWrite();
    tft.fillRoundRectGPU(w.x, w.y, w.w, w.h, r, fill);
    tft.drawRoundRectGPU(w.x, w.y, w.w, w.h, r, on);
    tft.endWrite();
    centerLabel(w.label, w.x + w.w / 2, w.y + w.h / 2, 2, txt);
}

static void drawLabel(const Widget &w)
{
    tft.setTextSize(3);
    tft.setTextColor(paletteColor(FG_IDX));
    tft.drawString(w.label ? w.label : "", w.x, w.y + (w.h - 24) / 2);
}

static void drawSlider(const Widget &w)
{
    uint16_t accent = fillColorOf(w);
    // Clear the whole slider rect first: a drag redraws every frame, so the
    // previous knob/fill must be erased or it smears across the track. Sliders
    // only appear on overlay screens, whose background is bgColor().
    tft.fillRectGPU(w.x, w.y, w.x + w.w - 1, w.y + w.h - 1, bgColor());

    int margin = w.h / 2;
    int lo = w.x + margin, hi = w.x + w.w - margin;
    int span = hi - lo;
    if (span < 1) span = 1;
    int cy = w.y + w.h / 2;
    int trackH = 8;

    int val = w.ivalue ? *w.ivalue : w.vmin;
    if (val < w.vmin) val = w.vmin;
    if (val > w.vmax) val = w.vmax;
    int denom = (w.vmax - w.vmin); if (denom == 0) denom = 1;
    int knobX = lo + (int)((int64_t)(val - w.vmin) * span / denom);

    tft.startWrite();
    // unfilled track (gray) then filled portion (accent)
    tft.fillRoundRectGPU(lo, cy - trackH / 2, span, trackH, trackH / 2, paletteColor(1));
    if (knobX - lo > 0)
        tft.fillRoundRectGPU(lo, cy - trackH / 2, knobX - lo, trackH, trackH / 2, accent);
    tft.fillCircleGPU(knobX, cy, margin - 4, accent);
    tft.endWrite();
}

static void drawCheckbox(const Widget &w)
{
    uint16_t accent = fillColorOf(w);
    if (!screenIsOverlay())
        tft.fillRectGPU(w.x, w.y, w.x + w.w - 1, w.y + w.h - 1, bgColor());

    int box = w.h - 10;
    int bx = w.x, by = w.y + 5;
    bool checked = w.bvalue && *w.bvalue;

    tft.startWrite();
    tft.drawRoundRectGPU(bx, by, box, box, 4, accent);
    if (checked)
        tft.fillRoundRectGPU(bx + 4, by + 4, box - 8, box - 8, 2, accent);
    tft.endWrite();

    tft.setTextSize(2);
    tft.setTextColor(paletteColor(FG_IDX));
    tft.drawString(w.label ? w.label : "", bx + box + 12, w.y + (w.h - 16) / 2);
}

static void drawList(const Widget &w)
{
    uint16_t accent = fillColorOf(w);
    uint16_t on     = textColorOf(w);
    int page = w.page ? *w.page : 0;
    int total = w.items ? (int)w.items->size() : 0;

    for (int r = 0; r < w.rows; r++) {
        int idx = page * w.rows + r;
        int ry = w.y + r * LIST_ROW_H;
        if (idx < total) {
            tft.startWrite();
            tft.fillRoundRectGPU(w.x, ry + 2, w.w, LIST_ROW_H - 6,
                                 (LIST_ROW_H - 6) / 4, accent);
            tft.drawRoundRectGPU(w.x, ry + 2, w.w, LIST_ROW_H - 6,
                                 (LIST_ROW_H - 6) / 4, on);
            tft.endWrite();
            centerLabel((*w.items)[idx].c_str(), w.x + w.w / 2,
                        ry + LIST_ROW_H / 2, 2, on);
        } else if (!screenIsOverlay()) {
            tft.fillRectGPU(w.x, ry, w.x + w.w - 1, ry + LIST_ROW_H - 1, bgColor());
        }
    }
}

void widgetDraw(const Widget &w, bool pressed)
{
    switch (w.type) {
        case W_BUTTON:   drawButton(w, pressed); break;
        case W_LABEL:    drawLabel(w);           break;
        case W_SLIDER:   drawSlider(w);          break;
        case W_CHECKBOX: drawCheckbox(w);        break;
        case W_LIST:     drawList(w);            break;
        default: break;
    }
}

int widgetListIndexAt(const Widget &w, int tx, int ty)
{
    if (tx < w.x || tx >= w.x + w.w) return -1;
    int row = (ty - w.y) / LIST_ROW_H;
    if (row < 0 || row >= w.rows) return -1;
    int page = w.page ? *w.page : 0;
    int idx = page * w.rows + row;
    int total = w.items ? (int)w.items->size() : 0;
    return (idx < total) ? idx : -1;
}

} // namespace ui
