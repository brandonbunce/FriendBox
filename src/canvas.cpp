#include "canvas.hpp"
#include "display.hpp"
#include "ui_core.hpp"

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
static bool wasTouching = false;

bool initCanvas()
{
    // Canvas storage now lives in LT7680 SDRAM (slot 0) - see initDisplay().
    // No ESP32 allocation needed. Kept as a distinct init step in case canvas
    // bookkeeping needs to grow back later.
    couldInitCanvasFrameBuffer = true;
    return true;
}

// Brush stroke: fills a circle of `radius` around (x, y) with `colorIndex`.
// Hardware-accelerated via the LT7680 Geometric Drawing Engine. The LGFX
// wrapper handles off-canvas clipping (and falls back to software for top/left
// overhang), so callers can pass raw touch coordinates.
void canvasDrawBrush(int x, int y, int radius, uint8_t colorIndex)
{
    tft.fillCircleGPU(x, y, radius, draw_color_palette[colorIndex]);
}

// Pencil stamp: square of side 2*radius+1 centred on (x, y). Same `radius`
// semantics as the brush so the two tools produce visually comparable stamp
// sizes for the same brush-size setting.
void canvasDrawPencil(int x, int y, int radius, uint8_t colorIndex)
{
    tft.fillRectGPU(x - radius, y - radius, x + radius, y + radius,
                    draw_color_palette[colorIndex]);
}

void canvasDrawDither(int x, int y, int radius, uint8_t colorIndex)
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
                if ((px % 2 == 0) && (py % 2 == 0) && px >= 0 && px < TFT_HOR_RES && py >= 0 && py < TFT_VER_RES)
                {
                    tft.drawPixel(px, py, color);
                }
            }
        }
    }
}

// Parametric line — stamps fn every radius/2 pixels from (x0,y0) to (x1,y1).
// Stepping by radius/2 gives smooth coverage with far fewer fills than 1-pixel Bresenham.
static void interpolateLine(int x0, int y0, int x1, int y1,
                            void (*fn)(int, int, int, uint8_t),
                            int radius, uint8_t colorIndex)
{
    int dx = x1 - x0, dy = y1 - y0;
    int step = max(1, radius / 2);
    int nsteps = max(0, (int)(sqrtf(dx * dx + dy * dy) / step));
    for (int i = 0; i <= nsteps; i++)
    {
        float t = nsteps > 0 ? (float)i / nsteps : 0.0f;
        fn(x0 + (int)(dx * t + 0.5f), y0 + (int)(dy * t + 0.5f), radius, colorIndex);
    }
}

// Same as interpolateLine but advances the rainbow palette index at each step.
static void interpolateRainbow(int x0, int y0, int x1, int y1, int radius)
{
    int dx = x1 - x0, dy = y1 - y0;
    int step = max(1, radius / 2);
    int nsteps = max(0, (int)(sqrtf(dx * dx + dy * dy) / step));
    for (int i = 0; i <= nsteps; i++)
    {
        float t = nsteps > 0 ? (float)i / nsteps : 0.0f;
        canvasDrawBrush(x0 + (int)(dx * t + 0.5f), y0 + (int)(dy * t + 0.5f),
                        radius, draw_rainbow_palette_index[currentRainbowPaletteIndex]);
        currentRainbowPaletteIndex = (currentRainbowPaletteIndex + 1) % 7;
    }
}

// 16 vertical colour stripes covering the whole canvas. Diagnostic only.
void drawTestPattern()
{
    tft.startWrite();
    for (uint8_t i = 0; i < 16; i++)
    {
        // Each stripe is 32 pixels wide (480 / 16 = 30 - close enough; the
        // (x>>5) version produced 15 full + 1 short stripe, keep that vibe).
        int x0 = i * 32;
        int w = (i == 15) ? (TFT_HOR_RES - x0) : 32;
        tft.fillRectGPU(x0, 0, w, TFT_VER_RES, draw_color_palette[i]);
    }
    tft.endWrite();
}

void drawClearScreen()
{
    tft.fillScreen(draw_color_palette[currentDrawColorIndex]);
}

void handleCanvasDraw()
{
    if (currentScreen == SCREEN_CANVAS && touchZ)
    {
        // On the first tick of a new stroke lastTouchX/Y hold stale values, so
        // start the line at the current point instead (draws a single dot).
        int x0 = wasTouching ? (int)lastTouchX : (int)touchX;
        int y0 = wasTouching ? (int)lastTouchY : (int)touchY;

        tft.startWrite();
        switch (currentTool)
        {
        case TOOL_PENCIL:
            interpolateLine(x0, y0, touchX, touchY, canvasDrawPencil, currentBrushRadius, currentDrawColorIndex);
            break;
        case TOOL_BRUSH:
            interpolateLine(x0, y0, touchX, touchY, canvasDrawBrush, currentBrushRadius, currentDrawColorIndex);
            break;
        case TOOL_FILL:
            drawClearScreen();
            break;
        case TOOL_RAINBOW: // Wouldn't be a bad idea to make this actually rainbow instead of cycling thru palette.... to follow ROYGBIV.
            interpolateRainbow(x0, y0, touchX, touchY, currentBrushRadius);
            break;
        case TOOL_DITHER:
            // Oh my god. why?
            interpolateLine(x0, y0, touchX, touchY, canvasDrawDither, currentBrushRadius, currentDrawColorIndex);
            break;
        case TOOL_STICKER:
            drawTestPattern();
            break;
        }
        tft.endWrite();
    }
    wasTouching = touchZ > 0;
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