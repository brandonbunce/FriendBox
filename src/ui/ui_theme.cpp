// System-wide accent theming. Single source of truth for widget colors so the
// per-screen `draw_color_palette[currentDrawColorIndex]` copy-paste disappears.
// The accent is just an index into the fixed 16-color CLUT, mirrored from the
// canvas module's currentDrawColorIndex so the sketch color and UI accent stay
// in lockstep.
#include "ui.hpp"
#include "canvas.hpp"
#include "lt_assets.hpp"

namespace ui {

// Glyphs are 32x32. Center the box; enlarge multiplies the footprint.
void glyphCentered(int glyphCode, int x, int y, int w, int h,
                   uint16_t fg565, uint8_t enlarge)
{
    int gw = 32 * enlarge, gh = 32 * enlarge;
    int gx = x + (w - gw) / 2;
    int gy = y + (h - gh) / 2;
    if (gx < 0) gx = 0;
    if (gy < 0) gy = 0;
    ltDrawGlyph((GlyphCode)glyphCode, gx, gy, fg565, /*transparentBg=*/true, enlarge);
}

uint16_t paletteColor(int idx)
{
    if (idx < 0) idx = 0;
    if (idx > 15) idx = 15;
    return draw_color_palette[idx];
}

uint16_t paletteTextColor(int idx)
{
    if (idx < 0) idx = 0;
    if (idx > 15) idx = 15;
    return draw_color_palette_text_color[idx];
}

int accentIndex() { return currentDrawColorIndex; }

uint16_t accentColor()   { return paletteColor(currentDrawColorIndex); }
uint16_t onAccentColor() { return paletteTextColor(currentDrawColorIndex); }

void setAccent(int paletteIdx)
{
    if (paletteIdx < 0) paletteIdx = 0;
    if (paletteIdx > 15) paletteIdx = 15;
    if (paletteIdx == currentDrawColorIndex) return;
    currentDrawColorIndex = paletteIdx;
    // Re-theme the live screen: every widget defaulting to the accent must
    // repaint. markAllDirty()/redraw() lives in ui.cpp.
    redraw();
}

} // namespace ui
