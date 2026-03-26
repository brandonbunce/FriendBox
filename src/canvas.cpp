#include "canvas.h"
#include "display.h"
#include "ui.h"

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

/* Stores our image drawing buffer. About ~76.8kb! */
uint8_t *canvas_framebuffer;

void drawPixelToFB(int x, int y, uint8_t colorIndex)
{
    if (x < 0 || x >= TFT_HOR_RES || y < 0 || y >= TFT_VER_RES)
        return;

    int index = y * TFT_HOR_RES + x;
    int byteIndex = index >> 1;

    if (index & 1)
        canvas_framebuffer[byteIndex] = (canvas_framebuffer[byteIndex] & 0xF0) | (colorIndex & 0x0F);
    else
        canvas_framebuffer[byteIndex] = (canvas_framebuffer[byteIndex] & 0x0F) | ((colorIndex & 0x0F) << 4);

    // This doesnt update display...
}

/** Draw a circle brush at x,y with given radius and color - Updates BOTH framebuffer and screen in real-time! */
void drawBrushToFB(int x, int y, int radius, uint8_t colorIndex)
{
    // Draw filled circle using midpoint circle algorithm
    for (int dy = -radius; dy <= radius; dy++)
    {
        for (int dx = -radius; dx <= radius; dx++)
        {
            // Check if point is inside circle
            if (dx * dx + dy * dy <= radius * radius)
            {
                int px = x + dx;
                int py = y + dy;

                // Draw to framebuffer
                drawPixelToFB(px, py, colorIndex);

                // Draw to screen immediately for instant feedback
                if (px >= 0 && px < TFT_HOR_RES && py >= 0 && py < TFT_VER_RES)
                {
                    tft.drawPixel(px, py, draw_color_palette[colorIndex]);
                }
            }
        }
    }
}

void drawDitherToFB(int x, int y, int radius, uint8_t colorIndex)
{
    // Draw filled circle using midpoint circle algorithm
    for (int dy = -radius; dy <= radius; dy++)
    {
        for (int dx = -radius; dx <= radius; dx++)
        {
            // Check if point is inside circle
            if (dx * dx + dy * dy <= radius * radius)
            {
                int px = x + dx;
                int py = y + dy;

                // Only draw even pixels.
                if ((px % 2 == 0) && py % 2 == 0)
                {
                    // Draw to framebuffer
                    drawPixelToFB(px, py, colorIndex);

                    // Draw to screen immediately for instant feedback
                    if (px >= 0 && px < TFT_HOR_RES && py >= 0 && py < TFT_VER_RES)
                    {
                        tft.drawPixel(px, py, draw_color_palette[colorIndex]);
                    }
                }
            }
        }
    }
}

void drawTest4()
{
    for (int y = 0; y < tft.height(); y++)
    {
        for (int x = 0; x < tft.width(); x++)
        {
            uint8_t color = (x >> 5) & 0x0F; // 16 vertical stripes
            drawPixelToFB(x, y, color);
        }
    }
}

void drawClearScreen()
{
    for (int y = 0; y < tft.height(); y++)
    {
        for (int x = 0; x < tft.width(); x++)
        {
            drawPixelToFB(x, y, currentDrawColorIndex);
        }
    }
    // updateDisplayWithFB();
    drawFramebuffer();
}

/** Draw to screen if within canvas context! */
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
            drawFramebuffer();
            break;
        }
    }
}

/* Change brush size while keeping brush size above 0.*/
void changeBrushSize(int targetValue)
{
    if (targetValue > 0 && targetValue <= 100)
    {
        Serial.print("Adjusting brush size to ");
        Serial.println(targetValue);
        currentBrushRadius = targetValue;
    }
}

// Helper functions to change tool settings
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