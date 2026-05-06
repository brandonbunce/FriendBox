#include "display.hpp"
#include "canvas.hpp"

// Display
LGFX tft;

// Touch
uint16_t touchX, touchY, touchZ; // Z:0 = no touch, Z>0 = touching
// Stores millis() value from last recorded input.
static unsigned long lastTouchTime = 0;
/** How many "inputs" should we drop after intial touch and liftoff? */
#define TOUCH_INPUT_BUFFER 10

bool initDisplay()
{
    if (!tft.init())
    {
        return false;
    }
    tft.setRotation(3); // This option enables suffering. Don't forget to account for coordinate translation!
    tft.setBrightness(255);
    tft.setColorDepth(16);
    return true;
}

void handleTouch()
{
    uint16_t localTouchX, localTouchY;
    if (tft.getTouch(&localTouchX, &localTouchY) && (localTouchX >= 0 && localTouchX < TFT_HOR_RES &&
                                                     localTouchY >= 0 && localTouchY < TFT_VER_RES))
    {
        // Touching in bounds
        /*Serial.print("Touch - X: ");
        //Serial.print(localTouchX);
        //Serial.print(" Y: ");
        //Serial.println(localTouchY);*/

        touchX = localTouchX;
        touchY = localTouchY;
        touchZ = 1;
    }
    else
    {
        lastTouchTime = millis();
        touchZ = 0;
    }
}