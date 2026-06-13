// LT7680 flash-resident assets: one-shot programming of embedded glyph + splash
// blobs into the external serial flash, loading glyphs into CGRAM, and rendering.
#include "lt_assets.hpp"
#include "display.hpp"
#include "canvas.hpp"
#include "idf_compat.hpp"

#include <esp_heap_caps.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "LTASSET";
static bool s_glyphs_ready = false;

// Marker = "FBXA" + version byte (+ pad). Written last so a power-loss mid-program
// leaves the marker absent and reprogramming retries on the next boot.
static bool markerValid()
{
    uint8_t m[6] = {0};
    displayFlashRead(LT_ASSET_MARKER_ADDR, m, sizeof(m));
    return m[0] == 'F' && m[1] == 'B' && m[2] == 'X' && m[3] == 'A' &&
           m[4] == LT_ASSET_VERSION;
}

static void programAssets()
{
    printf("[%s] programming LT7680 flash assets (one-time)...\n", TAG);

    displayFlashErase(LT_ASSET_GLYPH_ADDR, lt_glyph_data_len);
    displayFlashProgram(LT_ASSET_GLYPH_ADDR, lt_glyph_data, lt_glyph_data_len);

    if (lt_splash_w && lt_splash_h) {
        uint32_t sbytes = (uint32_t)lt_splash_w * lt_splash_h;
        displayFlashErase(LT_ASSET_SPLASH_ADDR, sbytes);
        displayFlashProgram(LT_ASSET_SPLASH_ADDR, lt_splash_data, sbytes);
    }

    uint8_t m[6] = { 'F', 'B', 'X', 'A', LT_ASSET_VERSION, 0 };
    displayFlashErase(LT_ASSET_MARKER_ADDR, sizeof(m));
    displayFlashProgram(LT_ASSET_MARKER_ADDR, m, sizeof(m));
    printf("[%s] flash assets programmed.\n", TAG);
}

static void loadGlyphsToCgram()
{
    if (lt_glyph_data_len == 0) return;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(lt_glyph_data_len, MALLOC_CAP_SPIRAM);
    if (!buf) { printf("[%s] glyph buffer alloc failed\n", TAG); return; }

    displayFlashRead(LT_ASSET_GLYPH_ADDR, buf, lt_glyph_data_len);
    tft.cgramSetStart(LT7680_CGRAM_ADDR);
    tft.cgramWrite(LT7680_CGRAM_ADDR, buf, lt_glyph_data_len);
    free(buf);

    s_glyphs_ready = true;
    printf("[%s] %u glyphs loaded into CGRAM @0x%06X\n", TAG,
           (unsigned)LT_GLYPH_COUNT, (unsigned)LT7680_CGRAM_ADDR);
}

/* Program LT7680 flash assets (one-time) + load glyphs into CGRAM. */
void initLTAssets()
{
    if (!markerValid()) programAssets();
    loadGlyphsToCgram();
}

/* DMA splash screen to LT7680.*/
void ltShowSplash()
{
    if (lt_splash_w && lt_splash_h && markerValid()) {
        int x = (TFT_HOR_RES - (int)lt_splash_w) / 2;
        int y = (TFT_VER_RES - (int)lt_splash_h) / 2;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        tft.startWrite();
        tft.fillScreen(draw_color_palette[currentDrawColorIndex]);
        tft.endWrite();
        displayDmaFlashToCanvas(LT_ASSET_SPLASH_ADDR, lt_splash_w, lt_splash_h,
                                LT7680_SLOT_CANVAS, (uint16_t)x, (uint16_t)y);
        return;
    }

    // Fallback: GPU-drawn wordmark.
    tft.startWrite();
    tft.fillScreen(draw_color_palette[currentDrawColorIndex]);
    tft.setTextColor(draw_color_palette_text_color[currentDrawColorIndex],
                     draw_color_palette[currentDrawColorIndex]);
    tft.setTextSize(6);
    tft.drawCenterString("DEV BRANCH", TFT_HOR_RES / 2, TFT_VER_RES / 2 - 24);
    tft.endWrite();
}

void ltDrawGlyph(GlyphCode code, int x, int y, uint16_t fg565,
                 bool transparentBg, uint8_t enlarge)
{
    if (!s_glyphs_ready) return;
    uint16_t bg = draw_color_palette[currentDrawColorIndex];
    tft.drawCharGPU((uint16_t)code, (uint16_t)x, (uint16_t)y, fg565, bg,
                    LT_GLYPH_HEIGHT_CODE, enlarge, transparentBg, /*UCG*/2);
}
