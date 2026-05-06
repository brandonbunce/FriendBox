#include "io.hpp"
#include "display.hpp"
#include "canvas.hpp"
#include "ui.hpp"

// Storage
Preferences nvs; // https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/nvs_flash.html
SPIClass sdspi = SPIClass(HSPI);

// File format on disk: 480*480 = 230,400 pixels packed two-per-byte as 4-bit
// palette indices, MSB nibble = even pixel, LSB nibble = odd pixel.
// Total file size: 115,200 bytes per saved sketch.
static constexpr size_t SKETCH_FILE_BYTES = (TFT_HOR_RES * TFT_VER_RES) / 2;
static constexpr size_t SKETCH_PACKED_BYTES_PER_ROW = TFT_HOR_RES / 2;

// Map an RGB565 colour read back from LT7680 SDRAM to its 4-bit palette index.
// Drawing only writes draw_color_palette[i] colours, so an exact match is
// expected; the nearest-distance fallback is just defensive against reads
// from regions that might have been touched by non-canvas drawing.
static uint8_t paletteIndexForColor(uint16_t color)
{
    for (uint8_t i = 0; i < 16; i++)
    {
        if (draw_color_palette[i] == color) return i;
    }
    uint8_t  best_idx  = 0;
    uint32_t best_dist = UINT32_MAX;
    int r = (color >> 11) & 0x1F;
    int g = (color >>  5) & 0x3F;
    int b =  color        & 0x1F;
    for (uint8_t i = 0; i < 16; i++)
    {
        uint16_t pc = draw_color_palette[i];
        int dr = ((pc >> 11) & 0x1F) - r;
        int dg = ((pc >>  5) & 0x3F) - g;
        int db = ( pc        & 0x1F) - b;
        uint32_t dist = (uint32_t)(dr * dr + dg * dg + db * db);
        if (dist < best_dist) { best_dist = dist; best_idx = i; }
    }
    return best_idx;
}

bool initNVS()
{
  nvs.begin("Friendbox", true);
  // loadImageFromSD(nvs.getUInt("lastActiveSlot", 8));
  nvs.end();
  return true;
}

bool initSD(bool forceFormat)
{
#ifdef FRIENDBOX_DEBUG_MODE
    Serial.println("INFO: Initializing SD...");
#endif
    sdspi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    sdspi.setFrequency(40000000); // 40 MHz, explicitly otherwise will take 80mhz speed of display bus and cause corruption (?)
    if (!SD.begin(SD_CS, sdspi))
    {
#ifdef FRIENDBOX_DEBUG_MODE
        Serial.println("ERROR: SD mount failed! Is it connected properly?");
#endif
        return false;
    }
    else
    {
        return true;
#ifdef FRIENDBOX_DEBUG_MODE
        Serial.println("INFO: SD ready!");
#endif
    }
}

bool initMenuButton() {
    pinMode(HALL_SENSOR_PIN, INPUT_PULLUP);
    return true;
}

// Stream the canvas to an open file, line-by-line: read 480 RGB565 pixels
// from LT7680 SDRAM, quantise each to its 4-bit palette index, pack two
// indices per byte, write 240 bytes. Result on disk = 115,200 bytes (same
// format as the pre-SDRAM-canvas era - existing saves stay loadable).
static bool writeCanvasToFile(File &f)
{
    static uint16_t lineBuf[TFT_HOR_RES];
    static uint8_t  packed[SKETCH_PACKED_BYTES_PER_ROW];
    for (int y = 0; y < TFT_VER_RES; y++)
    {
        tft.readRect(0, y, TFT_HOR_RES, 1, lineBuf);
        for (int x = 0; x < TFT_HOR_RES; x += 2)
        {
            uint8_t hi = paletteIndexForColor(lineBuf[x    ]) & 0x0F;
            uint8_t lo = paletteIndexForColor(lineBuf[x + 1]) & 0x0F;
            packed[x >> 1] = (hi << 4) | lo;
        }
        if (f.write(packed, SKETCH_PACKED_BYTES_PER_ROW) != SKETCH_PACKED_BYTES_PER_ROW)
        {
            return false;
        }
    }
    return true;
}

// Stream a 4-bit packed sketch from an open file into the LT7680 SDRAM
// display slot via tft.pushImage. Then snapshots to the backing slot so
// later UI overlay restores see the loaded canvas, not pre-load garbage.
static bool readCanvasFromFile(File &f)
{
    static uint8_t  packed[SKETCH_PACKED_BYTES_PER_ROW];
    static uint16_t lineBuf[TFT_HOR_RES];
    for (int y = 0; y < TFT_VER_RES; y++)
    {
        if (f.read(packed, SKETCH_PACKED_BYTES_PER_ROW) != SKETCH_PACKED_BYTES_PER_ROW)
            return false;
        for (int x = 0; x < TFT_HOR_RES; x += 2)
        {
            uint8_t b = packed[x >> 1];
            lineBuf[x    ] = draw_color_palette[(b >> 4) & 0x0F];
            lineBuf[x + 1] = draw_color_palette[ b       & 0x0F];
        }
        tft.writeRawPixels(0, y, TFT_HOR_RES, lineBuf);
    }
    snapshotCanvas();
    return true;
}

void saveImageToSD(int slot)
{
    drawFriendboxLoadingScreen("Saving...", 0);
    if ((slot + 1) > SLOT_DROPDOWN_BUTTON_COUNT || slot < 0)
    {
        // Invalid save slot.
        return;
    }
    char filename[50];
    snprintf(filename, sizeof(filename), "/sketches/slots/slot%d.fbox", slot);
    File f = SD.open(filename, FILE_WRITE);
    if (f)
    {
        bool ok = writeCanvasToFile(f);
        f.close();
        if (ok)
        {
            currentSaveSlot = slot;
            nvs.begin("Friendbox", false);
            nvs.putUInt("lastActiveSlot", currentSaveSlot);
            nvs.end();
            drawFriendboxLoadingScreen("Saved!", 250);
#ifdef FRIENDBOX_DEBUG_MODE
            Serial.print("Saved image to save slot ");
            Serial.print(slot);
            Serial.println("!");
#endif
        }
        else
        {
            drawFriendboxLoadingScreen("ERROR: WRITE TRUNCATED!", 1000);
        }
        drawFramebuffer();
    }
    else
    {
        drawFriendboxLoadingScreen("ERROR: SAVE FAILED!", 1000);
        drawFramebuffer();
    }
}

void loadSketchFromSD(const char *path)
{
    drawFriendboxLoadingScreen("Loading...", 0);
    char filename[50];
    snprintf(filename, sizeof(filename), "/sketches/saved/%s", path);
    Serial.println("Loading: ");
    Serial.println(filename);
    File f = SD.open(filename, FILE_READ);
    if (f)
    {
        readCanvasFromFile(f);
        f.close();
    }
    else
    {
        drawFriendboxLoadingScreen("Loading...", 500, "File Doesn't Exist :(");
    }
}

void loadImageFromSD(int slot)
{
    drawFriendboxLoadingScreen("Loading...", 0);
    if ((slot + 1) > SLOT_DROPDOWN_BUTTON_COUNT || slot < 0)
    {
        tft.print(slot);
        tft.println(" is not a valid save slot.");
        return;
    }
    char filename[50];
    snprintf(filename, sizeof(filename), "/sketches/slots/slot%d.fbox", slot);

    File f = SD.open(filename, FILE_READ);
    if (f)
    {
        readCanvasFromFile(f);
        f.close();
        currentSaveSlot = slot;
        nvs.begin("Friendbox", false);
        nvs.putUInt("lastActiveSlot", currentSaveSlot);
        nvs.end();
#ifdef FRIENDBOX_DEBUG_MODE
        Serial.print("Loaded image from save slot ");
        Serial.print(slot);
        Serial.println("!");
#endif
    }
    else
    {
        drawFriendboxLoadingScreen("No Sketch Saved!", 500);
        drawFramebuffer();
#ifdef FRIENDBOX_DEBUG_MODE
        Serial.print("Cant load slot ");
        Serial.print(slot);
        Serial.println(" as it does not exist.");
#endif
    }
}

void handleMenuButton(bool recheckInput)
{
    if (currentScreen == SCREEN_CANVAS || currentScreen == SCREEN_CANVAS_MENU)
    {
        static unsigned long lastPress = 0;
        static unsigned int lastButtonState = 0;
        static bool alreadyPressed = false;
        if (digitalRead(HALL_SENSOR_PIN) == LOW) /*Button Pressed*/
        {
            if (lastPress == 0)
            {
                lastPress = millis();
            }
            if (((millis() >= (lastPress + DEBOUNCE_MILLISECONDS)) & !alreadyPressed) || recheckInput)
            {
                Serial.println("Logical Press");
                changeScreenContext(SCREEN_CANVAS_MENU);
                alreadyPressed = true;
            }
            else
            {
                return;
            }
        }
        else /*Button Released*/
        {
            if (lastPress && alreadyPressed)
            {
                lastPress = 0;
                alreadyPressed = false;
                Serial.println("Logical Release.");
                if (currentScreen != SCREEN_CANVAS)
                    changeScreenContext(SCREEN_CANVAS);
            }
        }
    }
}

std::vector<std::string> sdGetFboxFiles()
{
    std::vector<std::string> fileNames;
    File root = SD.open("/sketches/saved");
    if (root)
    {
        File entry;
        while (entry = root.openNextFile())
        {
            if (!entry.isDirectory())
            {
                Serial.println("Found file: " + String(entry.name()));
                fileNames.push_back(entry.name());
            }
            entry.close();
        }
        root.close();
    }
    return fileNames;
}