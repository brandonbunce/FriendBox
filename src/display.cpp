#include "display.hpp"
#include "canvas.hpp"

// Display
LGFX tft;

// Touch
uint16_t touchX, touchY, touchZ; // Z:0 = no touch, Z>0 = touching
// Stores millis() value from last recorded input.
static unsigned long lastTouchTime = 0;
// For how many milliseconds after last input should we count before registering release?
#define UI_TOUCH_INPUT_BUFFER_MS
/** How many "inputs" should we drop after intial touch and liftoff? */
#define TOUCH_INPUT_BUFFER 10
/** Stores how many times we have registered a touch input. */
static uint16_t touch_count = 0;
/** Touch inputs waiting to be drawn (insane asylum) */
static uint16_t touch_queue_x[TOUCH_INPUT_BUFFER] = {0}, touch_queue_y[TOUCH_INPUT_BUFFER] = {0};
/** Last written value(s) in the touch queue. */
static uint8_t touch_queue_lastwrite_position = 0;

bool initDisplay()
{
#ifdef FRIENDBOX_DEBUG_MODE
    Serial.println("INFO: Initializing LGFX...");
#endif
    tft.init();
    tft.fillScreen(TFT_DARKCYAN);
    tft.setRotation(3); // This option enables suffering. Don't forget to account for coordinate translation!
    tft.setBrightness(255);
    tft.setColorDepth(16);

    // Allocate framebuffer in ROTATED dimensions
    canvas_framebuffer = (uint8_t *)malloc((tft.width() * tft.height()) / 4); // 76.8 KB
    if (!canvas_framebuffer)
    {
        Serial.println("FATAL: Framebuffer allocation failed!");
        while (1)
            ;
    }
    memset(canvas_framebuffer, 0, (tft.width() * tft.height()) / 4);
    return true;
}

bool initTouch(bool forceCalibrate)
{
#ifdef FRIENDBOX_DEBUG_MODE
    Serial.println("INFO: Initializing LGFX touch...");
#endif
    uint16_t calibration_data[8];
    bool calibration_data_ok = false;

    File touch_calibration_file = SD.open("/friendbox/touch_calibration_file.bin", FILE_READ);
    if (touch_calibration_file)
    { // File present, read and apply.
        if (touch_calibration_file.readBytes((char *)calibration_data, 16) == 16)
        {
            calibration_data_ok = true;
#ifdef FRIENDBOX_DEBUG_MODE
            Serial.println("INFO: Calibration Data OK!");
#endif
        }
        else
        {
#ifdef FRIENDBOX_DEBUG_MODE
            Serial.println("INFO: Calibration Data is incomplete or corrupted! Deleting...");
#endif
            SD.remove("/friendbox/touch_calibration_file.bin");
        }
        touch_calibration_file.close();
    }

    if (!calibration_data_ok || forceCalibrate)
    { // data not valid. recalibrate
#ifdef FRIENDBOX_DEBUG_MODE
        Serial.println("INFO: Recreating touchscreen calibration because:");
        Serial.print("calibration_data_ok: ");
        Serial.println(calibration_data_ok);
        Serial.print("forceCalibrate: ");
        Serial.println(forceCalibrate);
#endif
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setCursor(160, 40);
        tft.setTextSize(3);
        tft.drawCenterString("Touch Needs Calibration", 240, 70);
        tft.setTextSize(2);
        tft.drawCenterString("Press Highlighted Corners...", 240, 120);

        tft.calibrateTouch(calibration_data, TFT_WHITE, TFT_RED, 15);

#ifdef FRIENDBOX_DEBUG_MODE
        Serial.println("Touch Calibration Data");
        for (int i = 0; i < 8; i++)
        {
            Serial.println(calibration_data[i]);
        }
#endif
        File touch_calibration_file = SD.open("/friendbox/touch_calibration_file.bin", FILE_WRITE);
        if (touch_calibration_file)
        {
            touch_calibration_file.write((const unsigned char *)calibration_data, sizeof(calibration_data));
            touch_calibration_file.close();
#ifdef FRIENDBOX_DEBUG_MODE
            Serial.println("INFO: Successfully wrote calibration data to SD.");
#endif
        }
    }

    tft.setTouchCalibrate(calibration_data);
    return true;
}

void drawFramebuffer(int x, int y, int w, int h)
{
    int x1 = max(0, x);
    int y1 = max(0, y);
    int x2 = min((int)tft.width(), x + w);
    int y2 = min((int)tft.height(), y + h);

    int width = x2 - x1;
    int height = y2 - y1;

    if (width <= 0 || height <= 0)
        return;

    static uint16_t lineBuffer[TFT_HOR_RES];

    tft.startWrite();
    tft.setAddrWindow(x1, y1, width, height);

    for (int py = y1; py < y2; py++)
    {
        for (int px = x1; px < x2; px++)
        {
            int pixelIndex = py * tft.width() + px;
            int byteIndex = pixelIndex >> 1;
            uint8_t byte = canvas_framebuffer[byteIndex];
            uint8_t colorIndex;

            if (pixelIndex & 1)
            {
                colorIndex = byte & 0x0F;
            }
            else
            {
                colorIndex = (byte >> 4) & 0x0F;
            }

            lineBuffer[px - x1] = draw_color_palette[colorIndex];
        }

        // Try the version with swap parameter
        tft.writePixelsDMA(lineBuffer, width, true); // false = don't swap bytes
    }

    tft.endWrite();
}

void handleTouch()
{
    uint16_t localTouchX, localTouchY;
    if (tft.getTouch(&localTouchX, &localTouchY) && (localTouchX >= 0 && localTouchX < TFT_HOR_RES &&
                                                     localTouchY >= 0 && localTouchY < TFT_VER_RES))
    { // Touching in bounds
        Serial.print("Touch - X: ");
        Serial.print(localTouchX);
        Serial.print(" Y: ");
        Serial.println(localTouchY);

        // Drop inputs until we exceed TOUCH_INPUT_BUFFER, this prevents smearing from pen applying pressure.
        if (++touch_count > TOUCH_INPUT_BUFFER)
        {
            if (touch_queue_lastwrite_position < TOUCH_INPUT_BUFFER)
            {
                touch_queue_x[touch_queue_lastwrite_position] = localTouchX;
                touch_queue_y[touch_queue_lastwrite_position] = localTouchY;

                touch_queue_lastwrite_position =
                    (touch_queue_lastwrite_position + 1) % TOUCH_INPUT_BUFFER;
            }
            // Draw points in queue if valid.
            uint8_t touch_queue_read_position = (touch_queue_lastwrite_position + TOUCH_INPUT_BUFFER - (TOUCH_INPUT_BUFFER - 1)) % TOUCH_INPUT_BUFFER;

            // Serial.print("Last Write Pos: ");
            // Serial.println(touch_queue_lastwrite_position);
            // Serial.print("Read Pos: ");
            // Serial.println(touch_queue_read_position);
            // Serial.print("X Value: ");
            // Serial.println(touch_queue_x[touch_queue_read_position]);

            if (((touch_queue_x[touch_queue_read_position] +
                  touch_queue_y[touch_queue_read_position]) > 0))
            {
                touchX = touch_queue_x[touch_queue_read_position];
                touchY = touch_queue_y[touch_queue_read_position];
                touchZ = 1;
            }
        }
    }
    else
    { // No longer touching, re-init to zero. Also drops last 10 inputs to prevent smearing from pen removing pressure.
        touch_count = 0;
        memset(touch_queue_x, 0, sizeof(touch_queue_x));
        memset(touch_queue_y, 0, sizeof(touch_queue_y));
        lastTouchTime = millis();
        touchZ = 0;
    }
}