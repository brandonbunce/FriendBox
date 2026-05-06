#include "canvas.hpp"
#include "display.hpp"
#include "ui.hpp"

uint16_t draw_color_palette[16] = {
    0x0000, // Black (0)
    0x9CF3, // Gray (1)
    0xFFFF, // White (2)
    0xb926, // Red (3)
    0xdb71, // Meat (4)
    0x49e5, // Dark Brown (5)
    0xa324, // Brown (6)
    0xec46, // Orange (7)
    0xf70d, // Yellow (8)
    0x3249, // Dark Green (9)
    0x4443, // Green (10)
    0xa665, // Slime Green (11)
    0x1926, // Night Blue (12)
    0x02b0, // Sea Blue (13)
    0x351d, // Sky Blue (14)
    0xb6dd  // Cloud Blue (15)
};

uint16_t draw_rainbow_palette_index[7] = {
    3,
    7,
    8,
    10,
    14,
    13,
    4};

uint16_t draw_color_palette_text_color[16] = {
    0xFFFF, // Black (0)
    0xFFFF, // Gray (1)
    0x0000, // White (2)
    0xFFFF, // Red (3)
    0xFFFF, // Meat (4)
    0xFFFF, // Dark Brown (5)
    0xFFFF, // Brown (6)
    0xFFFF, // Orange (7)
    0x0000, // Yellow (8)
    0xFFFF, // Dark Green (9)
    0xFFFF, // Green (10)
    0xFFFF, // Slime Green (11)
    0xFFFF, // Night Blue (12)
    0xFFFF, // Sea Blue (13)
    0xFFFF, // Sky Blue (14)
    0x0000  // Cloud Blue (15)
};

draw_tool_id_t currentTool = TOOL_BRUSH;
int currentBackgroundColorIndex = 0;
int currentDrawColorIndex = 3;
int currentRainbowPaletteIndex = 0;
int currentBrushRadius = 5;
int currentSaveSlot = 0;
bool couldInitCanvasFrameBuffer = 0;

bool initCanvas() {
    // Canvas storage now lives in LT7680 SDRAM (slot 0) - see initDisplay().
    // No ESP32 allocation needed. Kept as a distinct init step in case canvas
    // bookkeeping needs to grow back later.
    couldInitCanvasFrameBuffer = true;
    return true;
}

// Brush stroke: fills a circle of `radius` around (x, y) with `colorIndex`.
// Pixels are written straight to LT7680 SDRAM via tft.drawPixel; the chip
// scans them out for "instant feedback". A snapshot to the backing slot is
// taken on touch release in handleTouch().
void drawBrushToFB(int x, int y, int radius, uint8_t colorIndex)
{
    uint16_t color = draw_color_palette[colorIndex];
    for (int dy = -radius; dy <= radius; dy++)
    {
        for (int dx = -radius; dx <= radius; dx++)
        {
            if (dx * dx + dy * dy <= radius * radius)
            {
                int px = x + dx;
                int py = y + dy;
                if (px >= 0 && px < TFT_HOR_RES && py >= 0 && py < TFT_VER_RES)
                {
                    tft.drawPixel(px, py, color);
                }
            }
        }
    }
}

void drawDitherToFB(int x, int y, int radius, uint8_t colorIndex)
{
    uint16_t color = draw_color_palette[colorIndex];
    for (int dy = -radius; dy <= radius; dy++)
    {
        for (int dx = -radius; dx <= radius; dx++)
        {
            if (dx * dx + dy * dy <= radius * radius)
            {
                int px = x + dx;
                int py = y + dy;
                if ((px % 2 == 0) && (py % 2 == 0)
                    && px >= 0 && px < TFT_HOR_RES && py >= 0 && py < TFT_VER_RES)
                {
                    tft.drawPixel(px, py, color);
                }
            }
        }
    }
}

// 16 vertical colour stripes covering the whole canvas. Diagnostic only.
void drawTest4()
{
    tft.startWrite();
    for (uint8_t i = 0; i < 16; i++)
    {
        // Each stripe is 32 pixels wide (480 / 16 = 30 - close enough; the
        // (x>>5) version produced 15 full + 1 short stripe, keep that vibe).
        int x0 = i * 32;
        int w  = (i == 15) ? (TFT_HOR_RES - x0) : 32;
        tft.fillRect(x0, 0, w, TFT_VER_RES, draw_color_palette[i]);
    }
    tft.endWrite();
}

void drawClearScreen()
{
    tft.fillScreen(draw_color_palette[currentDrawColorIndex]);
    snapshotCanvas();   // commit the cleared canvas to the backing slot
}

void handleCanvasDraw()
{
    if (currentScreen == SCREEN_CANVAS && touchZ)
    {
        switch (currentTool)
        {
        case TOOL_PENCIL:
            drawBrushToFB(touchX, touchY, currentBrushRadius, currentDrawColorIndex);
            break;
        case TOOL_BRUSH:
            drawBrushToFB(touchX, touchY, currentBrushRadius, currentDrawColorIndex);
            break;
        case TOOL_FILL:
            drawClearScreen();
            break;
        case TOOL_RAINBOW: // Wouldn't be a bad idea to make this actually rainbow instead of cycling thru palette.... to follow ROYGBIV.
            drawBrushToFB(touchX, touchY, currentBrushRadius, draw_rainbow_palette_index[currentRainbowPaletteIndex]);
            currentRainbowPaletteIndex = (currentRainbowPaletteIndex + 1) % 7;
            break;
        case TOOL_DITHER:
            // Oh my god. why?
            drawDitherToFB(touchX, touchY, currentBrushRadius, currentDrawColorIndex);
            break;
        case TOOL_STICKER:
            drawTest4();
            snapshotCanvas();
            break;
        }
    }
}

void changeBrushSize(int targetValue)
{
    if (targetValue > 0 && targetValue <= 100)
    {
        Serial.print("Adjusting brush size to ");
        Serial.println(targetValue);
        currentBrushRadius = targetValue;
    }
}

void setDrawColor(uint8_t colorIndex)
{
    if (colorIndex < 16)
    {
        Serial.print("Color set to: ");
        Serial.println(colorIndex);
        currentDrawColorIndex = colorIndex;
    }
}

void setBackgroundColor(uint8_t colorIndex)
{
    if (colorIndex < 16)
    {
        Serial.print("Background color set to: ");
        Serial.println(colorIndex);
        currentBackgroundColorIndex = colorIndex;
        tft.fillScreen(draw_color_palette[colorIndex]);
    }
}