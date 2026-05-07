#include "display.hpp"
#include "canvas.hpp"

LGFX tft;
uint16_t touchX, touchY, touchZ;
u_int16_t lastTouchX, lastTouchY;
static unsigned long lastTouchTime = 0;

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
        /*Serial.print("Touch - X: ");
        Serial.print(localTouchX);
        Serial.print(" Y: ");
        Serial.println(localTouchY);*/

        /* Place last touch coordinate into lastX/lastY for tracking movement changes. */
        lastTouchX = touchX;
        lastTouchY = touchY;

        /* Update current touch coordinate to newest input. */
        touchX = localTouchX;
        touchY = localTouchY;

        /* Mark as touching. */
        touchZ = 1;
    }
    else
    {
        /* Stash the last time we were touching the display.*/
        lastTouchTime = millis();

        /* Mark as no longer touching the display. */
        touchZ = 0;
    }
}