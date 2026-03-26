#ifndef CANVAS_H
#define CANVAS_H

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>

/** Defines color palette for our 4-bit color frame buffer.
 *
 * @param 0 Black
 * @param 1 Gray
 * @param 2 White
 * @param 3 Red
 * @param 4 Meat
 * @param 5 Dark Brown
 * @param 6 Brown
 * @param 7 Orange
 * @param 8 Yellow
 * @param 9 Dark Green
 * @param 10 Green
 * @param 11 Slime Green
 * @param 12 Night Blue
 * @param 13 Sea Blue
 * @param 14 Sky Blue
 * @param 15 Cloud Blue
 *
 * Color palette is from https://androidarts.com/palette/16pal.htm
 */
extern uint16_t draw_color_palette[16];
/* Since our color palette isn't a rainbow, we loop thru this for the closest thing.*/
extern uint16_t draw_rainbow_palette_index[7];
/** Defines text of color corresponding to palette for readability. */
extern uint16_t draw_color_palette_text_color[16];

/** Defines tools that we can use on the canvas. */
typedef enum
{
    TOOL_PENCIL,
    TOOL_BRUSH,
    TOOL_FILL,
    TOOL_RAINBOW,
    TOOL_DITHER,
    TOOL_STICKER
} draw_tool_id_t;

extern uint8_t *canvas_framebuffer;

extern draw_tool_id_t currentTool;
extern int currentBackgroundColorIndex;
extern int currentDrawColorIndex;
extern int currentRainbowPaletteIndex;
extern int currentBrushRadius;
extern int currentSaveSlot;

// Functions
void changeBrushSize(int targetValue);
void setDrawColor(uint8_t colorIndex);
void drawTest4();
void drawClearScreen();
void drawPixelToFB(int x, int y, uint8_t colorIndex);
void drawBrushToFB(int x, int y, int radius, uint8_t colorIndex);
void drawDitherToFB(int x, int y, int radius, uint8_t colorIndex);
void handleCanvasDraw();

#endif