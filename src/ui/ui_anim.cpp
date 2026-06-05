// Screen-enter animations, GPU-driven and region-scoped.
//
// The freshly built screen is composed into SLOT_ANIM (scratch) over the right
// backdrop, then revealed onto the shown slot (SLOT_CANVAS for direct screens,
// SLOT_UI for overlays) with a short stepped sequence of BTE kicks:
//   - slide  : one blitFrames per step at an eased offset (+ bg fill)
//   - fade   : one blitFramesAlpha per step (cross-fades old -> new)
//   - bounce : slide-from-bottom with an ease-out-back overshoot
//
// Continuous per-widget tweens (the tick path) are stubbed for a later phase;
// idle screens therefore issue zero redraws.
#include "ui_internal.hpp"
#include "display.hpp"
#include "idf_compat.hpp"

namespace ui {

static const int   STEPS    = 12;
static const int   STEP_MS  = 12;
static const float BACK_C1  = 1.70158f;

static inline float easeOutCubic(float p) { float q = 1.f - p; return 1.f - q * q * q; }
static inline float easeOutBack(float p)
{
    float c3 = BACK_C1 + 1.f, q = p - 1.f;
    return 1.f + c3 * q * q * q + BACK_C1 * q * q;
}

static const uint32_t SCRATCH = LT7680_SLOT_ANIM;

// Compose the new screen into the scratch slot. Backdrop is a bg fill (clean
// overlay / full screen) or a copy of the currently shown slot (snapshot
// overlay / direct), so widgets land over the right pixels before the reveal.
static void composeScratch()
{
    tft.setCanvasAddress(SCRATCH);
    if (fillsBg())
        tft.fillScreen(bgColor());
    else
        tft.blitFrames(shownSlot(), 0, 0, SCRATCH, 0, 0, TFT_HOR_RES, TFT_VER_RES);
    drawWidgets();
    tft.setCanvasAddress(shownSlot());   // draw target back to the live slot
}

static void blitSlideStep(int ox, int oy)
{
    const int W = TFT_HOR_RES, H = TFT_VER_RES;
    int srcX = ox < 0 ? -ox : 0;
    int srcY = oy < 0 ? -oy : 0;
    int dstX = ox > 0 ?  ox : 0;
    int dstY = oy > 0 ?  oy : 0;
    int w = W - (ox < 0 ? -ox : ox);
    int h = H - (oy < 0 ? -oy : oy);
    if (w <= 0 || h <= 0) return;

    tft.fillScreen(bgColor());           // active draw target == shown slot
    tft.blitFrames(SCRATCH, (uint16_t)srcX, (uint16_t)srcY,
                   shownSlot(), (uint16_t)dstX, (uint16_t)dstY,
                   (uint16_t)w, (uint16_t)h);
}

static void runSlide(AnimKind k)
{
    const int W = TFT_HOR_RES, H = TFT_VER_RES;
    for (int s = 1; s <= STEPS; s++) {
        float lin = (float)s / STEPS;
        float p = (k == ANIM_BOUNCE) ? easeOutBack(lin) : easeOutCubic(lin);
        if (k != ANIM_BOUNCE && p > 1.f) p = 1.f;
        float rem = 1.f - p;
        int ox = 0, oy = 0;
        switch (k) {
            case ANIM_SLIDE_FROM_LEFT:   ox = (int)(-rem * W); break;
            case ANIM_SLIDE_FROM_RIGHT:  ox = (int)( rem * W); break;
            case ANIM_SLIDE_FROM_TOP:    oy = (int)(-rem * H); break;
            case ANIM_SLIDE_FROM_BOTTOM:
            case ANIM_BOUNCE:            oy = (int)( rem * H); break;
            default: break;
        }
        blitSlideStep(ox, oy);
        delay(STEP_MS);
    }
    tft.blitFrames(SCRATCH, 0, 0, shownSlot(), 0, 0, (uint16_t)W, (uint16_t)H);
}

static void runFade()
{
    const int W = TFT_HOR_RES, H = TFT_VER_RES;
    for (int s = 1; s <= STEPS; s++) {
        uint8_t a = (uint8_t)((31 * s) / STEPS);
        // DT = SCRATCH*alpha + shown*(1-alpha): cross-fade old -> new.
        tft.blitFramesAlpha(SCRATCH, 0, 0,
                            shownSlot(), 0, 0,
                            shownSlot(), 0, 0,
                            (uint16_t)W, (uint16_t)H, a);
        delay(STEP_MS);
    }
    tft.blitFrames(SCRATCH, 0, 0, shownSlot(), 0, 0, (uint16_t)W, (uint16_t)H);
}

void animPlayEnter(AnimKind k)
{
    if (k == ANIM_NONE) {
        tft.setCanvasAddress(shownSlot());
        composeScreen();
        return;
    }
    composeScratch();
    if (k == ANIM_FADE_IN) runFade();
    else                   runSlide(k);
}

void animTick()   {}            // continuous tweens: later phase
bool animActive() { return false; }

} // namespace ui
