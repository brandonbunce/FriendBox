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

// Compose one slide frame (bg fill + the new screen at an eased offset) into an
// off-screen slot. The caller page-flips to it; nothing is drawn into the live
// slot, so there is no fill-then-blit flicker.
static void slideCompose(uint32_t dst, int ox, int oy)
{
    const int W = TFT_HOR_RES, H = TFT_VER_RES;
    int srcX = ox < 0 ? -ox : 0;
    int srcY = oy < 0 ? -oy : 0;
    int dstX = ox > 0 ?  ox : 0;
    int dstY = oy > 0 ?  oy : 0;
    int w = W - (ox < 0 ? -ox : ox);
    int h = H - (oy < 0 ? -oy : oy);

    tft.setCanvasAddress(dst);
    tft.fillScreen(bgColor());
    if (w > 0 && h > 0)
        tft.blitFrames(SCRATCH, (uint16_t)srcX, (uint16_t)srcY,
                       dst, (uint16_t)dstX, (uint16_t)dstY,
                       (uint16_t)w, (uint16_t)h);
}

static void runSlide(AnimKind k)
{
    const int W = TFT_HOR_RES, H = TFT_VER_RES;
    // Two off-screen present buffers to ping-pong. SLOT_ANIM_B and SLOT_MENU are
    // both idle on the live display outside fbox playback (no transition fires
    // mid-playback), and both are distinct from SCRATCH (SLOT_ANIM, holding the
    // new screen) and from SLOT_CANVAS (the underlying sketch an overlay
    // transition must preserve). NOTE: SLOT_MENU also backs the playback menu's
    // render cache, so transitions clobber it — playFboxAnimation invalidates
    // that cache (g_pbc_cache.valid) per call to compensate.
    const uint32_t PRESENT[2] = { LT7680_SLOT_ANIM_B, LT7680_SLOT_MENU };
    int pp = 0;
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
        uint32_t back = PRESENT[pp];
        slideCompose(back, ox, oy);      // build the frame off-screen
        displayPresentSlot(back);        // atomic VBlank-synced page flip
        pp ^= 1;
        delay(STEP_MS);
    }
    // Settle: land the fully-resolved new screen in the real shown slot (still
    // off-screen — a PRESENT buffer is live), then flip back to it.
    tft.setCanvasAddress(shownSlot());
    tft.blitFrames(SCRATCH, 0, 0, shownSlot(), 0, 0, (uint16_t)W, (uint16_t)H);
    displayPresentSlot(shownSlot());
}

static void runFade()
{
    const int W = TFT_HOR_RES, H = TFT_VER_RES;
    // Same off-screen ping-pong as runSlide. The old screen stays untouched in
    // shownSlot() for the whole loop, so it serves as the blend's fixed S0
    // source while SCRATCH (the new screen) is S1. The LT7680 opacity blends
    // DT = S0*(1-a) + S1*a, so a:0->31 dissolves old -> new (not the reverse the
    // old in-place version assumed).
    const uint32_t PRESENT[2] = { LT7680_SLOT_ANIM_B, LT7680_SLOT_MENU };
    int pp = 0;
    for (int s = 1; s <= STEPS; s++) {
        uint8_t a = (uint8_t)((31 * s) / STEPS);
        uint32_t back = PRESENT[pp];
        tft.blitFramesAlpha(shownSlot(), 0, 0,
                            SCRATCH, 0, 0,
                            back, 0, 0,
                            (uint16_t)W, (uint16_t)H, a);
        displayPresentSlot(back);
        pp ^= 1;
        delay(STEP_MS);
    }
    // Settle: pure new screen into the real shown slot (off-screen — a PRESENT
    // buffer is live), then flip back to it. Alpha tops out at 31/32, so this
    // also crisps up the final ~3% the dissolve can't reach.
    tft.setCanvasAddress(shownSlot());
    tft.blitFrames(SCRATCH, 0, 0, shownSlot(), 0, 0, (uint16_t)W, (uint16_t)H);
    displayPresentSlot(shownSlot());
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
