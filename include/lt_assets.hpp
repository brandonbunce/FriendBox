// LT7680 flash-resident assets: user-defined character (UCG) glyphs and a boot
// splash. Glyph + splash blobs are embedded in firmware and programmed into the
// LT7680's external serial flash once (marker-guarded); glyphs are then loaded
// into CGRAM and rendered by the hardware character engine, recolorable to any
// foreground color.
#ifndef LT_ASSETS_HPP
#define LT_ASSETS_HPP

#include <stdint.h>

// UCG glyph codes. Glyphs are 32x32 "full width" (height-code 2 → width 32 for
// codes >= 0x8000). The engine maps code (0x8000+i) to CGRAM_ADDR + i*128, so
// the enum starts at 0x8000 and each glyph is 128 bytes.
enum GlyphCode : uint16_t
{
    GLYPH_PLAY = 0x8000,
    GLYPH_PAUSE,
    GLYPH_STOP,
    GLYPH_LOOP,
    GLYPH_PLUS,
    GLYPH_MINUS,
    GLYPH_CHEV_L,
    GLYPH_CHEV_R,
    GLYPH_CHECK,
    GLYPH_X,
    GLYPH_SEND,
    GLYPH_FOLDER,
    GLYPH_HEART,
    GLYPH__END
};
#define LT_GLYPH_COUNT (GLYPH__END - GLYPH_PLAY)
#define LT_GLYPH_BYTES 128          // 32x32 dot-matrix = 128 bytes/glyph
#define LT_GLYPH_HEIGHT_CODE 2      // CCR0 bits[5:4]: 10b = 32 dots tall

// Embedded glyph dot-matrix (generated: src/lt_glyph_data.cpp).
extern const uint8_t  lt_glyph_data[];
extern const uint32_t lt_glyph_data_len;

// Embedded splash, 8bpp CLUT indices, w*h bytes (generated: src/lt_splash_data.cpp).
// w==0 || h==0 → no embedded splash; ltShowSplash() draws a GPU wordmark instead.
extern const uint16_t lt_splash_w;
extern const uint16_t lt_splash_h;
extern const uint8_t  lt_splash_data[];

// LT7680 external-flash asset map (W25Q128, separate from ESP firmware flash).
#define LT_ASSET_MARKER_ADDR  0x000000u   // "FBXA" + version, own 4 KB sector
#define LT_ASSET_GLYPH_ADDR   0x001000u
#define LT_ASSET_SPLASH_ADDR  0x010000u
#define LT_ASSET_VERSION      4

// Program embedded assets into LT7680 flash if the marker is missing/stale, then
// load glyphs into CGRAM. Call once at boot after the panel is initialized.
void initLTAssets();

// Paint the boot splash into SLOT_CANVAS (flash DMA if present, else GPU wordmark).
void ltShowSplash();

// Render a glyph at (x,y) into the current canvas slot, recolored to fg565.
void ltDrawGlyph(GlyphCode code, int x, int y, uint16_t fg565,
                 bool transparentBg = true, uint8_t enlarge = 1);

#endif // LT_ASSETS_HPP
